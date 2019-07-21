/* This file is part of ESDM.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with ESDM.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 * @brief ESDM module registry that keeps track of available backends.
 *
 */

#include <backends-data/posix/posix.h>
#include <esdm-internal.h>
#include <esdm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG_ENTER ESDM_DEBUG_COM_FMT("MODULES", "", "")
#define DEBUG(fmt, ...) ESDM_DEBUG_COM_FMT("MODULES", fmt, __VA_ARGS__)

#ifdef ESDM_HAS_POSIX
#  include "backends-data/posix/posix.h"
#  pragma message("Building ESDM with support for generic POSIX backend.")
#endif

#ifdef ESDM_HAS_KDSA
#  include "backends-data/kdsa/kdsa.h"
#  pragma message("Building ESDM with Kove XPD KDSA support.")
#endif

#ifdef ESDM_HAS_CLOVIS
#  include "backends-data/Clovis/clovis.h"
#  pragma message("Building ESDM with Clovis support.")
#endif

#ifdef ESDM_HAS_WOS
#  include "backends-data/WOS/wos.h"
#  pragma message("Building ESDM with WOS support.")
#endif

// TODO: remove define on
#define ESDM_HAS_MD_POSIX
#ifdef ESDM_HAS_MD_POSIX
#  include "backends-metadata/posix/md-posix.h"
#  pragma message("Building ESDM with support generic 'MD-POSIX' backend.")
#endif

#define ESDM_HAS_MONDODB
#ifdef ESDM_HAS_MONGODB
#  include "backends-metadata/mongodb/mongodb.h"
#  pragma message("Building ESDM with MongoDB support.")
#endif

esdm_modules_t *esdm_modules_init(esdm_instance_t *esdm) {
  ESDM_DEBUG(__func__);

  // Setup module registry
  esdm_modules_t *modules = NULL;
  esdm_backend_t *backend = NULL;

  modules = (esdm_modules_t *)malloc(sizeof(esdm_modules_t));

  esdm_config_backends_t *config_backends = esdm_config_get_backends(esdm);
  esdm_config_backend_t *b = NULL;

  esdm_config_backend_t *metadata_coordinator = esdm_config_get_metadata_coordinator(esdm);

  // Register metadata backend (singular)
  // TODO: This backend is meant as metadata coordinator in a hierarchy of MD (later)
  if (strncmp(metadata_coordinator->type, "metadummy", 9) == 0) {
    modules->metadata_backend = metadummy_backend_init(metadata_coordinator);
  }
#ifdef ESDM_HAS_MONGODB
  else if (strncmp(metadata_coordinator->type, "mongodb", 7) == 0) {
    modules->metadata_backend = mongodb_backend_init(metadata_coordinator);
  }
#endif
  else {
    ESDM_ERROR("Unknown metadata backend type. Please check your ESDM configuration.");
  }

  // Register data backends
  modules->data_backend_count = config_backends->count;
  modules->data_backends = malloc(config_backends->count * sizeof(esdm_backend_t *));

  int i;
  for (i = 0; i < config_backends->count; i++) {
    b = &(config_backends->backends[i]);

    DEBUG("Backend config: %d, %s, %s, %s\n", i,
    b->type,
    b->id,
    b->target);

    if (strncmp(b->type, "POSIX", 5) == 0) {
      backend = posix_backend_init(b);
    }
#ifdef ESDM_HAS_KDSA
    else if (strncasecmp(b->type, "KDSA", 6) == 0) {
      backend = kdsa_backend_init(b);
    }
#endif
#ifdef ESDM_HAS_CLOVIS
    else if (strncasecmp(b->type, "CLOVIS", 6) == 0) {
      backend = clovis_backend_init(b);
    }
#endif
#ifdef ESDM_HAS_WOS
    else if (strncmp(b->type, "WOS", 3) == 0) {
      backend = wos_backend_init(b);
    }
#endif
    else {
      ESDM_ERROR("Unknown backend type. Please check your ESDM configuration.");
    }
    backend->config = b;
    modules->data_backends[i] = backend;
  }

  esdm->modules = modules;
  return modules;
}

esdm_status esdm_modules_finalize(esdm_instance_t *esdm) {
  ESDM_DEBUG(__func__);

  // TODO: unregister and finalize modules in reverse order
  /*
	int i;
	for(i = module_count - 1 ; i >= 0; i--){
		modules[i]->finalize();
	}
	*/

  if (esdm->modules) {
    free(esdm->modules);
    esdm->modules = NULL;
  }

  return ESDM_SUCCESS;
}

esdm_status esdm_modules_get_by_type(esdm_module_type_t type, esdm_module_type_array_t **array) {
  ESDM_DEBUG(__func__);
  return ESDM_SUCCESS;
}
