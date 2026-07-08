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

#ifndef IHS_IHS_VERSION_H_
#define IHS_IHS_VERSION_H_

#include <stdint.h>

/*
 * The ihs_shared ABI version, laid out as (major << 16) | minor.
 *
 * Additive changes (new entry points; new trailing struct fields behind a
 * leading struct_size) bump the minor. Breaking changes bump the major and the
 * library SOVERSION. A plugin built against an older minor keeps working
 * against a newer library; a plugin requiring a newer major is rejected at
 * ihs_get_api().
 */
#define IHS_SHARED_ABI_MAJOR 1u
#define IHS_SHARED_ABI_MINOR 0u
#define IHS_SHARED_ABI_VERSION \
  ((IHS_SHARED_ABI_MAJOR << 16) | IHS_SHARED_ABI_MINOR)

#define IHS_ABI_MAJOR(v) ((uint32_t)(v) >> 16)
#define IHS_ABI_MINOR(v) ((uint32_t)(v) & 0xffffu)

#endif /* IHS_IHS_VERSION_H_ */
