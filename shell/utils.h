#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <locale>

#include <pwd.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

#include "constants.h"

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

    auto home_env = getenv("HOME");

    std::string tmp;
    if (home_env && *home_env) {
      auto home_env_raw = std::string(home_env);
      tmp = trim(home_env_raw, "\"");
    } else {
      setpwent();
      auto pw = getpwuid(getuid());
      endpwent();

      if (pw && pw->pw_dir)
        tmp = pw->pw_dir;
    }

    auto path = fml::paths::JoinPaths({tmp, kXdgApplicationDir});

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

    auto config_env = getenv("XDG_CONFIG_HOME");
    if (config_env && *config_env) {
      auto config_env_raw = std::string(config_env);
      auto clean = trim(config_env_raw, "\"");
      auto path =
          fml::paths::JoinPaths({clean, kXdgConfigDir, kApplicationName});
      config_home_dir = strdup(path.c_str());
    } else {
      config_home_dir = GetHomePath();
    }
    return config_home_dir;
  }
};
