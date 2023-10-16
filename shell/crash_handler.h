#pragma once

#include "fml/macros.h"

class CrashHandler {
 public:
  CrashHandler();

  ~CrashHandler();

  static void trigger_crash();

  FML_DISALLOW_COPY_AND_ASSIGN(CrashHandler);
};
