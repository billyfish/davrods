/**
 * \file
 * \brief     Davrods configuration.
 * \author    Chris Smeele
 * \copyright Copyright (c) 2016, Utrecht University
 *
 * This file is part of Davrods.
 *
 * Davrods is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Davrods is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Davrods.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _DAVRODS_CONFIG_H_
#define _DAVRODS_CONFIG_H_

#include "mod_davrods.h"

#include "theme.h"

/**
 * \brief Davrods per-directory config structure.
 */
typedef struct {
    const char *rods_host;
    uint16_t    rods_port;
    const char *rods_zone;
    const char *rods_default_resource;
    const char *rods_env_file;
    const char *rods_exposed_root; // Note: This is not necessarily a path, see below.
    size_t      rods_tx_buffer_size;
    size_t      rods_rx_buffer_size;

    enum {
        // Need to have something other than a bool to recognize an 'unset' state.
        DAVRODS_TMPFILE_ROLLBACK_YES = 1,
        DAVRODS_TMPFILE_ROLLBACK_NO,
    } tmpfile_rollback;

    const char *locallock_lockdb_path;

    enum {
        DAVRODS_AUTH_NATIVE = 1,
        DAVRODS_AUTH_PAM,
    } rods_auth_scheme;

    int rods_auth_ttl; // In hours.

    enum {
        //                               rods_exposed_root conf value => actual path used
        //                               ------------------------------------------------------------------------------
        DAVRODS_ROOT_CUSTOM_DIR = 1, //             <path>           => <path>
        DAVRODS_ROOT_ZONE_DIR,       //             Zone             => /<zone>
        DAVRODS_ROOT_HOME_DIR,       //             Home             => /<zone>/home (not the user's home collection!)
        DAVRODS_ROOT_USER_DIR,       //             User             => /<zone>/home/<user>
    } rods_exposed_root_type;

    int themed_listings;
    struct HtmlTheme theme;

    const char *davrods_api_path_s;
    const char *davrods_public_username_s;
    const char *davrods_public_password_s;

} davrods_dir_conf_t;

extern const command_rec davrods_directives[];

void *davrods_create_dir_config(apr_pool_t *p, char *dir);
void *davrods_merge_dir_config( apr_pool_t *p, void *base, void *overrides);

#endif /* _DAVRODS_CONFIG_H_ */
