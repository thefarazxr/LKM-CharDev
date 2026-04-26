/*
 * user/nvml_validate.cpp — Validate kernel telemetry against NVML
 *
 * WHY THE DATA WILL NEVER FULLY MATCH (lesson baked into output):
 * ──────────────────────────────────────────────────────────────────
 * Our kernel module                     NVML
 * ──────────────────────────────        ──────────────────────────────────────
 * Reads sysfs/hwmon — what nvidia.ko   Calls nvidia.ko directly and gets the
 * *chooses* to expose to the generic   full internal telemetry state: PMU
 * Linux subsystem (≈5 attributes).     counters, ECC, throttle bitmasks,
 *                                       per-engine clocks, NVLink, and more.
 *
 * Even shared metrics (temperature, power) may differ due to different
 * internal sampling points or averaging windows inside nvidia.ko.
 *
 * Build:
 *   Without NVML:  g++ -O2 -Wall -std=c++17 -o nvml_validate nvml_validate.cpp
 *   With NVML:     g++ -O2 -Wall -std=c++17 -DHAVE_NVML \
 *                      -o nvml_validate nvml_validate.cpp -lnvidia-ml
 */

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

extern "C" {
#include "../include/gpu_telem_uapi.h"
}

#ifdef HAVE_NVML
# include <nvml.h>
#endif

/* ── RAII wrappers ───────────────────────────────────────────────────────── */

#ifdef HAVE_NVML
struct NvmlGuard {
    nvmlDevice_t dev{};
    bool ok{false};

    NvmlGuard() {
        if (nvmlInit() != NVML_SUCCESS) return;
        if (nvmlDeviceGetHandleByIndex(0, &dev) != NVML_SUCCESS) {
            nvmlShutdown(); return;
        }
        ok = true;
    }
    ~NvmlGuard() { if (ok) nvmlShutdown(); }

    std::string name() const {
        char buf[96] = {};
        nvmlDeviceGetName(dev, buf, sizeof(buf));
        return buf;
    }
    std::optional<double>   temp_c()    const {
        unsigned v{}; return nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &v)
                             == NVML_SUCCESS ? std::optional{(double)v} : std::nullopt; }
    std::optional<unsigned> fan_pct()   const {
        unsigned v{}; return nvmlDeviceGetFanSpeed(dev, &v) == NVML_SUCCESS
                             ? std::optional{v} : std::nullopt; }
    std::optional<double>   power_w()   const {
        unsigned v{}; return nvmlDeviceGetPowerUsage(dev, &v) == NVML_SUCCESS
                             ? std::optional{v / 1000.0} : std::nullopt; }
    std::optional<unsigned> core_mhz()  const {
        unsigned v{}; return nvmlDeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &v)
                             == NVML_SUCCESS ? std::optional{v} : std::nullopt; }
    std::optional<unsigned> mem_mhz()   const {
        unsigned v{}; return nvmlDeviceGetClockInfo(dev, NVML_CLOCK_MEM, &v)
                             == NVML_SUCCESS ? std::optional{v} : std::nullopt; }
    std::optional<unsigned> gpu_util()  const {
        nvmlUtilization_t u{}; return nvmlDeviceGetUtilizationRates(dev, &u)
                             == NVML_SUCCESS ? std::optional{u.gpu} : std::nullopt; }
    std::optional<unsigned> mem_util()  const {
        nvmlUtilization_t u{}; return nvmlDeviceGetUtilizationRates(dev, &u)
                             == NVML_SUCCESS ? std::optional{u.memory} : std::nullopt; }
    std::optional<unsigned long long> throttle() const {
        unsigned long long v{};
        return nvmlDeviceGetCurrentClocksThrottleReasons(dev, &v)
               == NVML_SUCCESS ? std::optional{v} : std::nullopt; }
    std::optional<unsigned long long> ecc_single() const {
        unsigned long long v{};
        return nvmlDeviceGetTotalEccErrors(dev,
               NVML_MEMORY_ERROR_TYPE_CORRECTED, NVML_AGGREGATE_ECC, &v)
               == NVML_SUCCESS ? std::optional{v} : std::nullopt; }
    std::optional<unsigned long long> ecc_double() const {
        unsigned long long v{};
        return nvmlDeviceGetTotalEccErrors(dev,
               NVML_MEMORY_ERROR_TYPE_UNCORRECTED, NVML_AGGREGATE_ECC, &v)
               == NVML_SUCCESS ? std::optional{v} : std::nullopt; }
    std::optional<unsigned> perf_state() const {
        nvmlPStates_t p{}; return nvmlDeviceGetPerformanceState(dev, &p)
               == NVML_SUCCESS ? std::optional{(unsigned)p} : std::nullopt; }
};
#endif /* HAVE_NVML */

/* ── Formatting helpers ──────────────────────────────────────────────────── */

static void sep() {
    puts("  ──────────────────────────────────────────────────────────────────────");
}

template<typename T>
static std::string opt_str(const std::optional<T>& v, const char* fmt, const char* na = "unavailable") {
    if (!v) return na;
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *v);
    return buf;
}

static void row(const char* metric, const std::string& kernel_val,
                const std::string& nvml_val, const char* note = "") {
    printf("  %-28s  %-22s  %-22s  %s\n",
           metric, kernel_val.c_str(), nvml_val.c_str(), note);
}

/* ── Kernel device snapshot ──────────────────────────────────────────────── */

static int read_kernel_sample(const char* dev, gpu_sample& s) {
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n  (is gpu_telem.ko loaded?)\n",
                dev, strerror(errno));
        return -1;
    }
    ssize_t n = read(fd, &s, sizeof(s));
    close(fd);
    if (n != (ssize_t)sizeof(s)) {
        fprintf(stderr, "read: got %zd / %zu bytes\n", n, sizeof(s));
        return -1;
    }
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char* argv[]) {
    const char* dev = (argc > 1) ? argv[1] : "/dev/gpu_telem";

#ifdef HAVE_NVML
    NvmlGuard nvml;
    if (!nvml.ok) {
        fprintf(stderr, "NVML init failed — is the NVIDIA driver loaded?\n");
        return 1;
    }
    printf("NVML device : %s\n", nvml.name().c_str());
#else
    printf("NVML        : not available (built without -DHAVE_NVML)\n");
    printf("             Showing kernel-driver snapshot only.\n");
#endif
    printf("Kernel dev  : %s\n\n", dev);

    gpu_sample ks{};
    if (read_kernel_sample(dev, ks) < 0) return 1;

    auto k_temp  = (ks.temp_millideg != -1)
                   ? std::optional{ks.temp_millideg / 1000.0} : std::nullopt;
    auto k_power = (ks.power_uw > 0)
                   ? std::optional{ks.power_uw / 1e6} : std::nullopt;

    /* ── Comparison table ── */
    printf("  %-28s  %-22s  %-22s  %s\n",
           "Metric", "Kernel driver", "NVML", "Notes");
    sep();

#ifdef HAVE_NVML
    /* Temperature */
    {
        auto nv = nvml.temp_c();
        std::string note;
        if (k_temp && nv) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Δ %.1f °C", *k_temp - *nv);
            note = buf;
        }
        row("Temperature",
            opt_str(k_temp, "%.1f °C"),
            opt_str(nv,     "%u °C"),
            note.c_str());
    }

    /* Fan — units differ: kernel=RPM, NVML=percent */
    row("Fan speed",
        ks.fan_rpm ? std::to_string(ks.fan_rpm) + " RPM" : "unavailable",
        opt_str(nvml.fan_pct(), "%u %%"),
        "kernel=RPM  NVML=percent");

    /* Power */
    {
        auto nv = nvml.power_w();
        std::string note;
        if (k_power && nv) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Δ %.2f W", *k_power - *nv);
            note = buf;
        }
        row("Power draw",
            opt_str(k_power, "%.2f W"),
            opt_str(nv,      "%.2f W"),
            note.c_str());
    }

    row("Core frequency",
        ks.core_freq_mhz ? std::to_string(ks.core_freq_mhz) + " MHz" : "unavailable",
        opt_str(nvml.core_mhz(), "%u MHz"),
        "");

    row("Memory frequency",
        ks.mem_freq_mhz  ? std::to_string(ks.mem_freq_mhz)  + " MHz" : "unavailable",
        opt_str(nvml.mem_mhz(),  "%u MHz"),
        "kernel: often 0 on NVIDIA");

    /* Utilisation — nvidia.ko does NOT expose these via hwmon */
    row("GPU utilisation",
        "unavailable",
        opt_str(nvml.gpu_util(), "%u %%"),
        "nvidia.ko withholds from hwmon");
    row("Memory utilisation",
        "unavailable",
        opt_str(nvml.mem_util(), "%u %%"),
        "nvidia.ko withholds from hwmon");

    /* NVML-only metrics — structurally invisible via sysfs */
    sep();
    puts("\n  NVML-only metrics (not reachable via any sysfs/hwmon reader):");
    printf("  %-28s  %-22s\n", "Metric", "NVML");

    {
        auto v = nvml.throttle();
        char buf[32]; snprintf(buf, sizeof(buf), "0x%08llx", v.value_or(0));
        row("Throttle reasons", "unavailable", v ? buf : "unavailable",
            "proprietary bitmask");
    }
    row("ECC single-bit errors", "unavailable",
        opt_str(nvml.ecc_single(), "%llu"), "proprietary counter");
    row("ECC double-bit errors", "unavailable",
        opt_str(nvml.ecc_double(), "%llu"), "proprietary counter");
    {
        auto v = nvml.perf_state();
        std::string s = v ? ("P" + std::to_string(*v)) : "unavailable";
        row("Performance state", "unavailable", s, "P0=max perf, P8=idle");
    }

#else  /* no NVML */
    row("Temperature",    opt_str(k_temp,  "%.1f °C"), "n/a", "");
    row("Fan speed",      ks.fan_rpm ? std::to_string(ks.fan_rpm)+" RPM":"unavailable", "n/a", "");
    row("Power draw",     opt_str(k_power, "%.2f W"),  "n/a", "");
    row("Core frequency", ks.core_freq_mhz ? std::to_string(ks.core_freq_mhz)+" MHz":"unavailable", "n/a", "");
    row("Memory frequency",ks.mem_freq_mhz ? std::to_string(ks.mem_freq_mhz)+" MHz":"unavailable",  "n/a", "");
    row("GPU utilisation","unavailable", "n/a", "nvidia.ko withholds from hwmon");

    sep();
    puts("\n  Build with -DHAVE_NVML -lnvidia-ml to see NVML-only metrics:");
    puts("    throttle reasons, ECC error counts, performance state,");
    puts("    per-engine clocks, NVLink bandwidth, memory bandwidth util.");
#endif

    /* ── Lesson ── */
    sep();
    puts("\n  LESSON: why kernel driver and NVML data will never fully match\n");
    puts("  ① Access layer");
    puts("      Our module reads sysfs/hwmon — what nvidia.ko *chooses* to publish");
    puts("      to the generic Linux subsystem.  NVML calls nvidia.ko *directly*.");
    puts("");
    puts("  ② Vendor privilege");
    puts("      nvidia.ko has register-level GPU access: PMU counters, SM occupancy,");
    puts("      NVLink stats, ECC hardware — none of this surfaces via hwmon.");
    puts("");
    puts("  ③ Sampling skew");
    puts("      Even shared metrics differ: nvidia.ko uses different averaging");
    puts("      windows and internal sensor tap points than its hwmon node.");
    puts("");
    puts("  ④ What our driver IS good for");
    puts("      The kernel-side pipeline (ring buffer, chardev, wait_queue,");
    puts("      copy_to_user) and quantifying the *floor* of generic interfaces.");
    puts("      The gap vs NVML is the architectural lesson.\n");

    return 0;
}
