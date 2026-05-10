// postgresql_tuner.cpp
// PostgreSQL performance tuner for macOS
// Compile: clang++ -std=c++17 -o pg_tuner postgresql_tuner.cpp
// Usage:   ./pg_tuner [--output <file>] [--dry-run]

#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>

// -----------------------------------------------------------------------------
// System information retrieval (macOS specific)
// -----------------------------------------------------------------------------

uint64_t get_total_ram_bytes() {
    uint64_t memsize = 0;
    size_t size = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &size, nullptr, 0) != 0) {
        std::cerr << "Warning: failed to get total RAM, using default 2GB" << std::endl;
        return 2ULL * 1024 * 1024 * 1024;
    }
    return memsize;
}

int get_cpu_cores() {
    int ncpu = 0;
    size_t size = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &size, nullptr, 0) != 0) {
        std::cerr << "Warning: failed to get CPU cores, assuming 2 cores" << std::endl;
        return 2;
    }
    return ncpu;
}

// Detect if root filesystem is on SSD (simple heuristic using diskutil)
// Returns true if likely SSD, false otherwise.
bool is_ssd() {
    // Modern Macs almost always use SSDs. We'll check using `diskutil info /`
    // If the command fails or "Solid State" not found, assume SSD (optimistic).
    FILE* pipe = popen("diskutil info / | grep -i 'Solid State'", "r");
    if (!pipe) return true;   // fallback
    char buffer[128];
    bool found = false;
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        if (strstr(buffer, "Yes") != nullptr) found = true;
    }
    pclose(pipe);
    return found;
}

// -----------------------------------------------------------------------------
// PostgreSQL configuration parameter calculation
// -----------------------------------------------------------------------------

struct PgConfig {
    int max_connections;
    int shared_buffers_mb;
    int effective_cache_size_mb;
    int maintenance_work_mem_mb;
    int work_mem_kb;
    int wal_buffers_mb;          // -1 means auto (set to -1)
    int min_wal_size_mb;
    int max_wal_size_mb;
    double checkpoint_completion_target;
    double random_page_cost;
    int effective_io_concurrency;
    int default_statistics_target;
    std::string log_line_prefix;
};

PgConfig compute_config(uint64_t total_ram_bytes, int cpu_cores, bool ssd) {
    const uint64_t MB = 1024 * 1024;
    uint64_t total_ram_mb = total_ram_bytes / MB;

    // 1. max_connections: conservative based on RAM
    int max_conn = (total_ram_mb >= 2048) ? 100 : 50;
    if (total_ram_mb <= 512) max_conn = 20;        // very low memory
    else if (total_ram_mb <= 1024) max_conn = 50;

    // 2. shared_buffers: 25% of RAM, up to 16GB
    int shared_buffers = static_cast<int>(std::round(total_ram_mb * 0.25));
    shared_buffers = std::min(shared_buffers, 16384);   // cap at 16GB
    shared_buffers = std::max(shared_buffers, 128);     // at least 128MB

    // 3. effective_cache_size: 75% of RAM
    int effective_cache = static_cast<int>(std::round(total_ram_mb * 0.75));

    // 4. maintenance_work_mem: 5% of RAM, up to 1GB
    int maint_work_mem = static_cast<int>(std::round(total_ram_mb * 0.05));
    maint_work_mem = std::min(maint_work_mem, 1024);
    maint_work_mem = std::max(maint_work_mem, 64);     // minimum 64MB

    // 5. work_mem: (25% of RAM) / max_connections, limited between 4MB and 256MB
    double work_mem_mb = (total_ram_mb * 0.25) / max_conn;
    work_mem_mb = std::min(work_mem_mb, 256.0);
    work_mem_mb = std::max(work_mem_mb, 4.0);
    int work_mem_kb = static_cast<int>(work_mem_mb * 1024);

    // 6. wal_buffers: auto (-1) for most cases, but set to 16MB if shared_buffers > 1GB
    int wal_buffers_mb = -1;
    if (shared_buffers > 1024) wal_buffers_mb = 16;

    // 7. WAL size limits: typical for mid-range systems
    int min_wal_size = 1024;   // 1GB
    int max_wal_size = 4096;   // 4GB, increase for larger RAM
    if (total_ram_mb >= 8192) max_wal_size = 8192;
    if (total_ram_mb >= 16384) max_wal_size = 16384;

    // 8. Checkpoint target: 0.9
    double checkpoint_target = 0.9;

    // 9. Disk type: SSD 1.1, HDD 4.0
    double rand_page_cost = ssd ? 1.1 : 4.0;

    // 10. effective_io_concurrency: SSD=200, HDD=2
    int io_concurrency = ssd ? 200 : 2;

    // 11. default_statistics_target: typical 100
    int stats_target = 100;

    // 12. log_line_prefix: include timestamp, pid, user, database
    std::string log_prefix = "'%t [%p] %q%u@%d '";

    return PgConfig{
        max_conn,
        shared_buffers,
        effective_cache,
        maint_work_mem,
        work_mem_kb,
        wal_buffers_mb,
        min_wal_size,
        max_wal_size,
        checkpoint_target,
        rand_page_cost,
        io_concurrency,
        stats_target,
        log_prefix
    };
}

void print_config(const PgConfig& cfg, std::ostream& out) {
    out << "# ------------------------------------------------------------\n";
    out << "# PostgreSQL configuration generated automatically by pg_tuner\n";
    out << "# Based on system resources: RAM, CPU cores, disk type\n";
    out << "# ------------------------------------------------------------\n\n";

    out << "# Connections and memory\n";
    out << "max_connections = " << cfg.max_connections << "\n";
    out << "shared_buffers = " << cfg.shared_buffers_mb << "MB\n";
    out << "effective_cache_size = " << cfg.effective_cache_size_mb << "MB\n";
    out << "maintenance_work_mem = " << cfg.maintenance_work_mem_mb << "MB\n";
    out << "work_mem = " << cfg.work_mem_kb << "kB\n";
    out << "wal_buffers = ";
    if (cfg.wal_buffers_mb == -1)
        out << "-1\n";
    else
        out << cfg.wal_buffers_mb << "MB\n";

    out << "\n# Write-Ahead Log (WAL)\n";
    out << "min_wal_size = " << cfg.min_wal_size_mb << "MB\n";
    out << "max_wal_size = " << cfg.max_wal_size_mb << "MB\n";

    out << "\n# Checkpoints\n";
    out << "checkpoint_completion_target = " << cfg.checkpoint_completion_target << "\n";

    out << "\n# I/O and parallelism\n";
    out << "random_page_cost = " << cfg.random_page_cost << "\n";
    out << "effective_io_concurrency = " << cfg.effective_io_concurrency << "\n";

    out << "\n# Planner and statistics\n";
    out << "default_statistics_target = " << cfg.default_statistics_target << "\n";

    out << "\n# Logging\n";
    out << "log_line_prefix = " << cfg.log_line_prefix << "\n";
}

// -----------------------------------------------------------------------------
// Command line parsing
// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string output_file;
    bool dry_run = false;

    // Simple argument parsing
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: " << argv[0] << " [--output <file>] [--dry-run]\n";
            std::cout << "  --output <file>   Write configuration to <file>\n";
            std::cout << "  --dry-run         Print configuration to stdout (no file write)\n";
            return 0;
        }
    }

    // Gather system resources
    uint64_t total_ram = get_total_ram_bytes();
    int cpu_cores = get_cpu_cores();
    bool ssd = is_ssd();

    std::cout << "System detection:\n"
              << "  Total RAM: " << (total_ram / (1024*1024*1024)) << " GB\n"
              << "  CPU cores: " << cpu_cores << "\n"
              << "  Storage:   " << (ssd ? "SSD" : "HDD") << "\n\n";

    PgConfig config = compute_config(total_ram, cpu_cores, ssd);

    if (dry_run) {
        print_config(config, std::cout);
    } else if (!output_file.empty()) {
        std::ofstream ofs(output_file);
        if (!ofs) {
            std::cerr << "Error: cannot open file " << output_file << " for writing\n";
            return 1;
        }
        print_config(config, ofs);
        std::cout << "Configuration written to " << output_file << "\n";
    } else {
        // No output specified, print to stdout
        print_config(config, std::cout);
    }

    return 0;
}
