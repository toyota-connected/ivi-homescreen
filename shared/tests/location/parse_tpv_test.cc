/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Unit test for GpsdProvider::ParseTpv — the gpsd JSON parsing that was split
// out from the socket precisely so it could be tested against captured output.
// Compiles the provider source directly (ParseTpv is internal, not part of the
// C ABI). No socket, gpsd, or D-Bus needed.

#include <cstdio>
#include <string>

#include "gpsd_provider.hpp"

using ihs::location::ParseTpv;
using ihs::location::Position;

namespace {
int g_fails = 0;
void Expect(bool cond, const char* what) {
  std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) {
    ++g_fails;
  }
}
}  // namespace

int main() {
  Position p;

  // A real gpsd 3D fix with track + speed.
  const bool r =
      ParseTpv(R"({"class":"TPV","device":"/dev/pts/3","mode":3,)"
               R"("lat":37.774900,"lon":-122.419400,"alt":12.3,"track":95.4,)"
               R"("speed":1.34})",
               p);
  Expect(r && p.valid() && p.mode == 3, "TPV 3D parses and is valid");
  Expect(p.latitude > 37.7 && p.latitude < 37.8, "latitude ~37.77");
  Expect(p.longitude < -122.4 && p.longitude > -122.5, "longitude ~-122.42");
  Expect(p.has_bearing && p.bearing_deg > 95.0 && p.bearing_deg < 96.0,
         "track 95.4 -> bearing");
  Expect(p.speed_mps > 1.3 && p.speed_mps < 1.4, "speed 1.34");

  // Error estimates: per-axis epx/epy/eps map to the sigma fields.
  Position e;
  Expect(ParseTpv(R"({"class":"TPV","mode":3,"lat":37.77,"lon":-122.42,)"
                  R"("epx":4.5,"epy":6.0,"eph":7.1,"eps":0.8})",
                  e),
         "TPV with error estimates parses");
  Expect(e.sigma_e_m > 4.4 && e.sigma_e_m < 4.6, "epx 4.5 -> sigma_e");
  Expect(e.sigma_n_m > 5.9 && e.sigma_n_m < 6.1, "epy 6.0 -> sigma_n");
  Expect(e.sigma_v_mps > 0.7 && e.sigma_v_mps < 0.9, "eps 0.8 -> sigma_v");

  // Only eph present: it fills both position axes.
  Position h;
  Expect(
      ParseTpv(
          R"({"class":"TPV","mode":3,"lat":37.77,"lon":-122.42,"eph":9.0})", h),
      "TPV with only eph parses");
  Expect(h.sigma_e_m > 8.9 && h.sigma_e_m < 9.1 && h.sigma_n_m > 8.9 &&
             h.sigma_n_m < 9.1,
         "eph fills both sigma_e and sigma_n");

  // No error fields: sigmas stay unknown (< 0), and speed defaults unknown too.
  Position n;
  Expect(ParseTpv(R"({"class":"TPV","mode":3,"lat":37.77,"lon":-122.42})", n),
         "TPV with no error fields parses");
  Expect(n.sigma_e_m < 0.0 && n.sigma_n_m < 0.0 && n.sigma_v_mps < 0.0,
         "absent error fields -> sigmas < 0 (unknown)");

  // A garbage/negative error is rejected, not stored.
  Position g;
  Expect(ParseTpv(R"({"class":"TPV","mode":3,"lat":37.77,"lon":-122.42,)"
                  R"("epx":-1.0})",
                  g) &&
             g.sigma_e_m < 0.0,
         "negative epx -> sigma_e unknown");

  // ParseTpv is pure: it must not stamp a timestamp (that is done at receipt).
  Expect(e.t_monotonic_ns == 0, "ParseTpv leaves t_monotonic_ns unstamped");

  // Non-TPV classes are rejected.
  Expect(!ParseTpv(R"({"class":"VERSION","release":"3.22"})", p),
         "VERSION rejected");
  Expect(!ParseTpv(R"({"class":"SKY","satellites":[]})", p), "SKY rejected");

  // TPV acquiring (mode 1, no position) is rejected.
  Expect(!ParseTpv(R"({"class":"TPV","mode":1})", p),
         "TPV mode 1 (no position) rejected");

  // TPV 2D with a position is accepted.
  Expect(ParseTpv(R"({"class":"TPV","mode":2,"lat":51.5,"lon":-0.12})", p) &&
             p.mode == 2,
         "TPV 2D accepted");

  // TPV with lat/lon but no mode is treated as a fix.
  Position q;
  Expect(ParseTpv(R"({"class":"TPV","lat":48.85,"lon":2.35})", q) && q.valid(),
         "TPV without mode assumes a fix");

  // Malformed input.
  Expect(!ParseTpv("not json", p), "garbage rejected");
  Expect(!ParseTpv("", p), "empty rejected");

  // Out-of-range / non-finite coordinates are rejected.
  Expect(!ParseTpv(R"({"class":"TPV","mode":3,"lat":91.0,"lon":0.0})", p),
         "latitude > 90 rejected");
  Expect(!ParseTpv(R"({"class":"TPV","mode":3,"lat":0.0,"lon":-181.0})", p),
         "longitude < -180 rejected");
  Expect(!ParseTpv(R"({"class":"TPV","mode":3,"lat":nan,"lon":0.0})", p),
         "NaN latitude rejected");
  Expect(!ParseTpv(R"({"class":"TPV","mode":3,"lat":0.0,"lon":inf})", p),
         "Inf longitude rejected");

  // Only mode 2/3 are fixes: a valid position with mode 4 or 1 is rejected.
  Expect(!ParseTpv(R"({"class":"TPV","mode":4,"lat":10.0,"lon":10.0})", p),
         "mode 4 rejected even with a position");
  Expect(!ParseTpv(R"({"class":"TPV","mode":1,"lat":10.0,"lon":10.0})", p),
         "mode 1 rejected even with a position");

  std::printf(g_fails ? "\n%d FAILED\n" : "\nALL PASS\n", g_fails);
  return g_fails == 0 ? 0 : 1;
}
