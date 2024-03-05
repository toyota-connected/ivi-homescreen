#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <locale>

#include <pwd.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

#include <flutter/encodable_value.h>

#include "constants.h"
#include "fml/paths.h"
#include "logging/logging.h"

/* switch private and public declarations */
#ifdef UNIT_TEST
#define PRIVATE public
#define PROTECTED public
#else
#define PRIVATE private
#define PROTECTED protected
#endif

class Utils {
 public:
  /**
   * @brief trim from end of string (right)
   * @return std::string&
   * @retval String that has specified characters trimmed from right.
   * @relation
   * flutter
   */
  inline static std::string& rtrim(std::string& s, const char* t) {
    s.erase(s.find_last_not_of(t) + 1);
    return s;
  }

  /**
   * @brief trim from beginning of string (left)
   * @return std::string&
   * @retval String that has specified characters trimmed from left.
   * @relation
   * flutter
   */
  inline static std::string& ltrim(std::string& s, const char* t) {
    s.erase(0, s.find_first_not_of(t));
    return s;
  }

  /**
   * @brief trim from both ends of string (right then left)
   * @return std::string&
   * @retval String that has specified characters trimmed from right and left.
   * @relation
   * flutter
   */
  inline static std::string& trim(std::string& s, const char* t) {
    return ltrim(rtrim(s, t), t);
  }

  /**
   * @brief Gets Home Path
   * @return const char*
   * @retval Home path string
   * @relation
   * flutter
   */
  static const char* GetHomePath() {
    static char* home_dir = nullptr;

    if (home_dir != nullptr)
      return home_dir;

    const auto home_env = getenv("HOME");

    std::string tmp;
    if (home_env && *home_env) {
      auto home_env_raw = std::string(home_env);
      tmp = trim(home_env_raw, "\"");
    } else {
      setpwent();
      const auto pw = getpwuid(getuid());
      endpwent();

      if (pw && pw->pw_dir)
        tmp = pw->pw_dir;
    }

    const auto path = fml::paths::JoinPaths({tmp, kXdgApplicationDir});

    home_dir = strdup(path.c_str());

    return home_dir;
  }

  /**
   * @brief Gets Config Home Path
   * @return const char*
   * @retval Home path string
   * @relation
   * flutter
   */
  static const char* GetConfigHomePath() {
    static const char* config_home_dir = nullptr;

    const auto config_env = getenv("XDG_CONFIG_HOME");
    if (config_env && *config_env) {
      auto config_env_raw = std::string(config_env);
      auto clean = trim(config_env_raw, "\"");
      const auto path =
          fml::paths::JoinPaths({clean, kXdgConfigDir, kApplicationName});
      config_home_dir = strdup(path.c_str());
    } else {
      config_home_dir = GetHomePath();
    }
    return config_home_dir;
  }

  /**
   * @brief Check if input is a number
   * @param[in] s String to check if it is a number
   * @return bool
   * @retval true If s is a number
   * @retval false If s is not a number
   * @relation
   * internal
   */
  static bool IsNumber(const std::string& s) {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return isdigit(c) != 0; });
  }

  /**
   * @brief Remove argument from vector
   * @param[in] args Vector of element that matches the argument to be removed
   * @param[in] arg Arguments to be removed
   * @return void
   * @relation
   * internal
   */
  static void RemoveArgument(std::vector<std::string>& args,
                             const std::string& arg) {
    const auto result = std::find(args.begin(), args.end(), arg);
    if (result != args.end()) {
      args.erase(result);
    }
  }

  /**
   * @brief Split string by token
   * @return std::vector<std::string>
   * @relation
   * internal
   */
  static std::vector<std::string> split(std::string str,
                                        const std::string& token) {
    std::vector<std::string> result;
    while (!str.empty()) {
      const auto index = str.find(token);
      if (index != std::string::npos) {
        result.push_back(str.substr(0, index));
        str = str.substr(index + token.size());
        if (str.empty())
          result.push_back(str);
      } else {
        result.push_back(str);
        str.clear();
      }
    }
    return result;
  }
};
