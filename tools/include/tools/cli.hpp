/// \file cli.hpp
/// \brief The argument parser and provenance header every tool shares.
///
/// The provenance line is not decoration. A benchmark CSV or an event dump
/// found six weeks later is worthless unless it says which build produced it,
/// how it was timed, and whether the observability tap was compiled in. Every
/// tool prints it to stderr -- stderr, so that piping stdout to a file still
/// gets a clean artefact.
#pragma once

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/timestamp.hpp"

#ifndef MINIEX_GIT_SHA
#define MINIEX_GIT_SHA "unknown"
#endif
#ifndef MINIEX_BUILD_TYPE
#define MINIEX_BUILD_TYPE "unknown"
#endif

namespace miniex {

class Cli {
public:
  Cli(std::string name, std::string summary)
      : name_(std::move(name)), summary_(std::move(summary)) {}

  /// Declares an option. `def` is used when the flag is absent; an empty
  /// `def` with `required` set makes the flag mandatory.
  void option(std::string flag, std::string help, std::string def = "",
              bool required = false) {
    order_.push_back(flag);
    help_[flag] = std::move(help);
    values_[flag] = std::move(def);
    if (required) required_.push_back(flag);
  }

  /// Declares a boolean flag, present or absent.
  void flag(std::string name, std::string help) {
    order_.push_back(name);
    help_[name] = std::move(help);
    flags_[name] = false;
  }

  /// Returns false if the program should exit (help requested or bad args).
  bool parse(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        print_help();
        return false;
      }
      if (arg.rfind("--", 0) != 0) {
        std::cerr << name_ << ": unexpected argument '" << arg << "'\n";
        print_help();
        return false;
      }
      const std::string key = arg.substr(2);
      if (flags_.count(key) != 0) {
        flags_[key] = true;
        continue;
      }
      if (values_.count(key) == 0) {
        std::cerr << name_ << ": unknown option '--" << key << "'\n";
        print_help();
        return false;
      }
      if (i + 1 >= argc) {
        std::cerr << name_ << ": --" << key << " needs a value\n";
        return false;
      }
      values_[key] = argv[++i];
    }
    for (const std::string& r : required_) {
      if (values_[r].empty()) {
        std::cerr << name_ << ": --" << r << " is required\n";
        print_help();
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] const std::string& get(const std::string& key) const {
    return values_.at(key);
  }
  [[nodiscard]] bool has(const std::string& key) const {
    return flags_.at(key);
  }
  [[nodiscard]] long long get_int(const std::string& key,
                                  long long fallback) const {
    const std::string& v = values_.at(key);
    if (v.empty()) return fallback;
    return std::strtoll(v.c_str(), nullptr, 10);
  }

  void print_help() const {
    std::cerr << name_ << " -- " << summary_ << "\n\nOptions:\n";
    for (const std::string& key : order_) {
      const bool is_flag = flags_.count(key) != 0;
      std::string left = "  --" + key + (is_flag ? "" : " <value>");
      left.resize(left.size() < 34 ? 34 : left.size(), ' ');
      std::cerr << left << help_.at(key);
      const auto it = values_.find(key);
      if (it != values_.end() && !it->second.empty())
        std::cerr << " (default: " << it->second << ")";
      std::cerr << "\n";
    }
    std::cerr << "  --help                            this message\n";
  }

  /// One line to stderr, before any output. Never omit it.
  static void print_provenance(const CycleCalibration* cal = nullptr) {
    std::cerr << "# miniex build=" << MINIEX_BUILD_TYPE
              << " sha=" << MINIEX_GIT_SHA
              << " timing=" << CycleCalibration::backend() << " tap="
#ifdef MINIEX_TAP
              << "ON"
#else
              << "OFF"
#endif
        ;
    if (cal != nullptr) {
      std::cerr << " ticks_per_s=" << cal->ticks_per_second()
                << " tick_ns=" << cal->cycles_to_ns(1)
                << (cal->coarse() ? " RESOLUTION=COARSE(development-only)"
                                  : "");
    }
    std::cerr << "\n";
  }

private:
  std::string name_;
  std::string summary_;
  std::vector<std::string> order_;
  std::vector<std::string> required_;
  std::map<std::string, std::string> help_;
  std::map<std::string, std::string> values_;
  std::map<std::string, bool> flags_;
};

} // namespace miniex
