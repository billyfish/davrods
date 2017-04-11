/**
 * \file
 * \brief     Davrods DAV repository.
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
#include "repo.h"

#include <http_request.h>
#include <http_protocol.h>

#include "theme.h"
#include "auth.h"

/************************************/

#ifdef DAVRODS_ENABLE_PROVIDER_LOCALLOCK
// TODO: Move locklocal_get_locked_entries declaration.
#include "lock_local.h"
#endif /* DAVRODS_ENABLE_PROVIDER_LOCALLOCK */


typedef struct walker_seen_resource_t
{
	const char *rods_path;
	struct walker_seen_resource_t *next;
} walker_seen_resource_t;


static const char *get_rods_root (apr_pool_t *davrods_pool, request_rec *r);
static int walker_push_seen_path (apr_pool_t *p, walker_seen_resource_t **seen, const char *rods_path);
static dav_error *dav_repo_get_resource (request_rec *r, const char *root_dir, const char *label, int use_checked_in, dav_resource **result_resource);

APLOG_USE_MODULE (davrods);

const char *GetRodsExposedPath (request_rec *req_p)
{
	const char *exposed_path_s = NULL;
	apr_pool_t *pool_p = GetDavrodsMemoryPool (req_p);

	if (pool_p)
		{
			exposed_path_s = get_rods_root (pool_p, req_p);
		} /* if (pool_p) */

	return exposed_path_s;
}

/**
 * \brief Get a pointer to the last part of a pathname.
 *
 * This does not change the path string, and as such will not remove any
 * trailing slashes.
 *
 * \param path
 *
 * \return a pointer pointing within path
 */
const char *get_basename (const char *path)
{
	size_t len = strlen (path);
	if (!len)
		return path;

	for (size_t i = len - 1; i > 0; i --)
		{
			if (i < len - 1 && path [i] == '/')
				{
					WHISPER("Translated path <%s> to basename <%s>\n", path, &path[i+1]);
					return path + i + 1;
				}
		}

	return path;
}

struct dav_repo_walker_private
{
	const dav_walk_params *params;
	dav_walk_resource wres;
	char uri_buffer [MAX_NAME_LEN + 2];
	dav_resource resource;
};

struct dav_stream
{
	apr_pool_t *pool;

	dataObjInp_t open_params;
	openedDataObjInp_t data_obj;
	bytesBuf_t output_buffer;
	const dav_resource *resource;

	char *write_path;

	char *container;
	size_t container_size;
	size_t container_off;
};

static dav_error *set_rods_path_from_uri (dav_resource *resource)
{
	// Set iRODS path and relative URI properties of the resource context based on the resource URI.

	const char *uri = resource->uri;

	// Chop root_dir off of the uri if applicable.
	if (resource->info->root_dir && strlen (resource->info->root_dir) > 1)
		{
			// We expect the URI to contain the specified root_dir (<- the Location in which davrods runs).
			if (strstr (uri, resource->info->root_dir) == uri)
				{
					uri += strlen (resource->info->root_dir);
				}
			else
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
							"Assertion failure: Root dir <%s> not in URI <%s>.",
							resource->info->root_dir, uri);
					return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
							0, "Malformed internal path");
				}
		}

	resource->info->relative_uri = uri;

	const char *rods_root = resource->info->rods_root;
	char *prefixed_path =
			rods_root ?
					apr_pstrcat (resource->pool, rods_root, uri, NULL) :
					apr_pstrdup (resource->pool, uri);

	if (strlen (prefixed_path) >= MAX_NAME_LEN)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
					"Generated an iRODS path exceeding iRODS path length limits for URI <%s>",
					uri);
			return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
					"Request URI too long");
		}

	int status = parseRodsPathStr (prefixed_path, resource->info->rods_env,
			resource->info->rods_path);
	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
					"Could not translate URI <%s> to an iRODS path: %s", uri,
					get_rods_error_msg (status));
			return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
					"Could not parse URI.");
		}

	ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, resource->info->r,
			"Mapped URI <%s> to rods path <%s>", uri, resource->info->rods_path);

	return NULL;
}

/**
 * \brief Copy private resource info to a new resource, discarding resource-specific information.
 *
 * \param[out] dest
 * \param[in]  src
 *
 * \return 
 */
static void copy_resource_context (dav_resource_private *dest,
		const dav_resource_private *src)
{
	*dest = *src;
	dest->rods_path [0] = '\0';
	dest->stat = NULL;
}

static const char *get_rods_root (apr_pool_t *davrods_pool, request_rec *r)
{
	const char *root = NULL;

	davrods_dir_conf_t *conf = ap_get_module_config (r->per_dir_config,
			&davrods_module);

	if (conf)
		{
			if (conf->rods_exposed_root_type == DAVRODS_ROOT_ZONE_DIR)
				{
					root = apr_pstrcat (davrods_pool, "/", conf->rods_zone,
					NULL);
				}
			else if (conf->rods_exposed_root_type == DAVRODS_ROOT_HOME_DIR)
				{
					root = apr_pstrcat (davrods_pool, "/", conf->rods_zone, "/home",
					NULL);
				}
			else if (conf->rods_exposed_root_type == DAVRODS_ROOT_USER_DIR)
				{
					const char *username_s = GetUsernameFromPool (davrods_pool);

					if (username_s)
						{
							root = apr_pstrcat (davrods_pool, "/", conf->rods_zone, "/home/", username_s, NULL);
						}
					else
						{
							ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_EGENERAL, r, "get_rods_root: failed to get username");
						}
				}
			else
				{
					root = conf->rods_exposed_root;
				}

			WHISPER("Determined rods root to be <%s> for this user (conf said <%s>)\n", root, conf->rods_exposed_root);
		}
	else
		{
			ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_EGENERAL, r,
					"get_rods_root: failed to get module config");
		}

	return root;
}

static apr_status_t rods_stat_cleanup (void *mem)
{
	WHISPER("Freeing rods stat struct @%p\n", mem);
	freeRodsObjStat ((rodsObjStat_t*) mem);
	return 0;
}

/**
 * \brief Query iRODS for information pertaining to a certain resource and fill in those resource properties.
 */
static dav_error *get_dav_resource_rods_info (dav_resource *resource)
{
	dav_resource_private *res_private = resource->info;
	request_rec *r = res_private->r;

	dav_error *err = set_rods_path_from_uri (resource);
	if (err)
		return err;

	dataObjInp_t obj_in = { { 0 } };
	rodsObjStat_t *stat_out = NULL;

	strcpy (obj_in.objPath, res_private->rods_path);
	int status = rcObjStat (res_private->rods_conn, &obj_in, &stat_out);

	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r,
					"Could not stat object <%s>: %s", res_private->rods_path,
					get_rods_error_msg (status));

			if (status == USER_FILE_DOES_NOT_EXIST)
				{
					resource->exists = 0;
				}
			else
				{
					return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
							0, "Could not stat object");
				}
		}
	else
		{
			res_private->stat = stat_out;
			apr_pool_cleanup_register (resource->pool, stat_out, rods_stat_cleanup,
					apr_pool_cleanup_null);

			ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r,
					"Object <%s> is a %s and has size %" DAVRODS_SIZE_T_FMT,
					res_private->rods_path,
					stat_out->objType == DATA_OBJ_T ? "data object" :
					stat_out->objType == COLL_OBJ_T ? "collection" : "thingy",
					stat_out->objSize);

			resource->exists = 1;

			if (stat_out->objType == DATA_OBJ_T)
				{
					resource->collection = 0;
				}
			else if (stat_out->objType == COLL_OBJ_T)
				{
					resource->collection = 1;
				}
			else
				{
					// This should not happen, but we're going to keep it from causing
					// issues anyways.
					resource->exists = 0;

					ap_log_rerror (APLOG_MARK, APLOG_WARNING, APR_SUCCESS, r,
							"Unknown iRODS object type <%d> for path <%s>! Will act as if it does not exist.",
							stat_out->objType, res_private->rods_path);
				}
		}

	return NULL;
}



struct dav_error *FillInPrivateResourceData (request_rec *req_p, dav_resource **resource_pp, const char *root_dir_s)
{
	dav_error *err_p = NULL;

	// Create private resource context {{{
	struct dav_resource_private *res_private_p = apr_pcalloc (req_p -> pool, sizeof (struct dav_resource_private));

	if (res_private_p)
		{
			// Create private resource context {{{
			res_private_p -> r = req_p;

			// Collect properties to insert into the resource context.

			// Get module config.
			res_private_p -> conf = ap_get_module_config (req_p -> per_dir_config, &davrods_module);


			if (res_private_p -> conf)
				{
					// Obtain iRODS connection.
					res_private_p -> davrods_pool = GetDavrodsMemoryPool (req_p);


					if (res_private_p -> davrods_pool)
						{
							res_private_p -> rods_conn = GetIRODSConnectionFromPool (res_private_p -> davrods_pool);

							if (! (res_private_p -> rods_conn))
								{
					        /*
					         * For publicly-accessible iRODS instances, check_rods will never have been called, so we'll need
					         * to get the memory pool and iRODS connection for the public user.
					         */

									if (res_private_p -> conf -> davrods_public_username_s)
										{
											authn_status status = GetIRodsConnection (req_p, & (res_private_p -> rods_conn), res_private_p -> conf -> davrods_public_username_s, res_private_p -> conf -> davrods_public_password_s ? res_private_p -> conf -> davrods_public_password_s : "");

											if (status != 0)
												{
													ap_log_rerror (__FILE__, __LINE__, APLOG_MODULE_INDEX, APLOG_ERR, APR_ECONNREFUSED, req_p, "error %d: Failed to connect to iRODS as public user \"%s\"", status, res_private_p -> conf -> davrods_public_username_s);

													WHISPER ("GetIRodsConnection failed for anonymous user");
												}
										}
								}

							if (res_private_p -> rods_conn)
								{
									// Obtain iRODS environment.
									if ((res_private_p -> rods_env = GetRodsEnvFromPool (res_private_p -> davrods_pool)) != NULL)
										{
											struct dav_resource *resource_p = NULL;

											// Get iRODS exposed root dir.
											res_private_p -> rods_root = get_rods_root (res_private_p -> davrods_pool, req_p);
											res_private_p -> root_dir = root_dir_s;

											// }}}
											// Create DAV resource {{{

											if ((resource_p = apr_pcalloc (req_p -> pool, sizeof (struct dav_resource))) != NULL)
												{
													resource_p -> uri = req_p -> uri;
													resource_p -> type = DAV_RESOURCE_TYPE_REGULAR;
													resource_p -> hooks = &davrods_hooks_repository;
													resource_p -> pool = req_p -> pool;
													resource_p -> info = res_private_p;

													*resource_pp = resource_p;
												} /* if ((resource_p = apr_pcalloc (req_p -> pool, sizeof (dav_resource))) != NULL) */

										} /* if ((res_private_p -> rods_env = GetRodsEnvFromPool (res_private_p -> davrods_pool)) != NULL) */

								} /*if (res_private_p -> rods_conn) */
							else
								{
									err_p = dav_new_error (req_p -> pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to get connect to irods");
								}
						}
					else
						{
							WHISPER("NO POOL!!!");
						}
				}
			else
				{
					err_p = dav_new_error (req_p -> pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to get module config");
				}

		}		/* if (res_private) */
	else
		{
			err_p = dav_new_error (req_p -> pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to allocate memory for resource");
		}

	return err_p;
}


/**
 * \brief Create a DAV resource struct for the given request URI.
 *
 * \param      r               the request that lead to this resource
 * \param      root_dir        the root URI where Davrods lives (its <Location>)
 * \param      label           unused
 * \param      use_checked_in  unused
 * \param[out] result_resource
 *
 * \return 
 */
static dav_error *dav_repo_get_resource (request_rec *r, const char *root_dir,
		const char *label, int use_checked_in, dav_resource **result_resource)
{
	dav_resource *resource_p = NULL;
	dav_error *err_p = FillInPrivateResourceData (r, &resource_p, root_dir);

	if (!err_p)
		{
			err_p = get_dav_resource_rods_info (resource_p);

			if (!err_p)
				{
					*result_resource = resource_p;
				}
		}

	return err_p;
}

static dav_error *dav_repo_get_parent_resource (const dav_resource *resource,
		dav_resource **result_parent)
{
	dav_error *err_p = NULL;

	WHISPER("Attempting to get parent resource of <%s>\n", resource->uri);

	// We should be able to make this assumption.
	if (* (resource -> uri) == '/')
		{
			if (strcmp (resource->uri, "/") == 0)
				{
					*result_parent = NULL; // We are already at the root directory.
				}
			else
				{
					// Generate a resource for the parent collection.
					dav_resource *parent = apr_pcalloc(resource->pool, sizeof(dav_resource));

					if (parent)
						{
							parent->info = apr_pcalloc(resource->pool, sizeof(dav_resource_private));

							if (parent->info)
								{
									char *uri = NULL;
									copy_resource_context (parent->info, resource->info);

									if ((uri = apr_pstrdup (resource->pool, resource->uri)) != NULL)
										{
											size_t uri_len = strlen (uri);
											uri [uri_len - 1] = '\0';
											// We already established that uri_len must be greater that 1.
											for (int i = uri_len - 2; i >= 0; i --)
												{
													// TODO: Verify that double slashes do not cause problems
													// here (are they filtered by apache / mod_dav?).
													if (uri [i] == '/')
														break;
													uri [i] = '\0';
												}
											parent->uri = uri;

											WHISPER("Parent of <%s> resides at <%s>\n", resource->uri, parent->uri);
											parent->type = DAV_RESOURCE_TYPE_REGULAR;
											parent->hooks = &davrods_hooks_repository;
											parent->pool = resource->pool;

											err_p = get_dav_resource_rods_info (parent);

											*result_parent = parent;
										}
									else
										{
											err_p = dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to allocate memory for copying resource uri");
										}
								}
							else
								{
									err_p = dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to allocate memory for parent resource info");
								}
						}
					else
						{
							err_p = dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to allocate memory for parent resource");
						}

				}

		}		/* if (* (resource -> uri) == '/') */
	else
		{
			char *error_s = apr_pstrcat (resource -> pool, "resource -> uri \"", resource -> uri, "\" does not begin with /", NULL);
			err_p = dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, error_s);
		}

	return err_p;
}

static int dav_repo_is_same_resource (const dav_resource *resource1,
		const dav_resource *resource2)
{
	if (resource1->hooks != resource2->hooks)
		{
			// This shouldn't happen since we always set the same hooks on our
			// resources. Unless mod_dav gives us a resource belonging to a different
			// DAV provider of course...
			return 0;
		}

	dav_resource_private *res_private1 = resource1->info;
	dav_resource_private *res_private2 = resource2->info;

	// iRODS handled path canonicalisation for us. We can safely compare these.
	return strcmp (res_private1->rods_path, res_private2->rods_path) == 0;
}

static int dav_repo_is_parent_resource (const dav_resource *parent,
		const dav_resource *child)
{
	if (parent->hooks != child->hooks)
		return 0;

	const char *path_parent = parent->info->rods_path;
	const char *path_child = child->info->rods_path;

	size_t pathlen_parent = strlen (path_parent);
	size_t pathlen_child = strlen (path_child);

	// This check is kind of fuzzy but sufficient.
	// It does the same thing as dav_fs' is_parent_resource().
	return pathlen_child > pathlen_parent + 1
			&& memcmp (path_parent, path_child, pathlen_parent) == 0
			&& path_child [pathlen_parent] == '/';
}

static dav_error *dav_repo_open_stream (const dav_resource *resource,
		dav_stream_mode mode, dav_stream **result_stream)
{
	dav_error *err_p = NULL;
	struct dav_stream *stream = apr_pcalloc(resource->pool, sizeof(dav_stream));

	if (stream)
		{
			stream->pool = resource->pool;
			stream->resource = resource;

			if (mode == DAV_MODE_WRITE_SEEKABLE
					|| (mode == DAV_MODE_WRITE_TRUNC
							&& resource->info->conf->tmpfile_rollback
									== DAVRODS_TMPFILE_ROLLBACK_NO))
				{
					// Either way, do not use tmpfiles for rollback support.
					stream->write_path = apr_pstrdup (stream->pool,
							resource->info->rods_path);

				}
			else if (mode == DAV_MODE_WRITE_TRUNC)
				{
					// If the TmpfileRollback config option is set, we create a temporary file when in truncate mode.

					// Think up a semi-random filename that's unlikely to exist in this directory.
					int cheapsum = 0;
					for (const char *c = resource->uri; c; c ++)
						cheapsum += 1 << *c;

					// Get the path to the parent directory.
					// TODO: Statting the parent collection is overkill, create a separate function for this.
					dav_resource *parent;

					err_p = dav_repo_get_parent_resource (resource, &parent);
					if (err_p)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_EGENERAL, resource->info->r,
									"Getting parent resource of <%s> failed in open_stream()",
									resource->uri);
							return err_p;
						}

					// XXX:  This assumes we have write access to the collection containing
					//       the resource, not just the data object itself, which may not
					//       always be the case.
					stream->write_path = apr_psprintf (stream->pool,
							"%s/.davrods-tx-%04x-%08lx", parent->info->rods_path, getpid (),
							time (NULL) ^ cheapsum);
				}
			else
				{
					// No other modes exist in mod_dav at this time.
					ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_EGENERAL, resource->info->r, "Unknown dav_stream_mode %d", mode);

					return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Unknown dav_stream_mode");
				}

			if (stream -> write_path)
				{
					if (strlen (stream->write_path) >= MAX_NAME_LEN)
						{
							// This can only happen in the temporary file case - the check on the
							// destination file name length happened during create_resource().
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
									"Generated a temporary filename exceeding iRODS MAX_NAME_LEN limits: <%s>. Aborting open_stream().",
									stream->write_path);
							return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
									"Could not generate a temporary filename within path length bounds");
						}

					dataObjInp_t *open_params = &stream->open_params;
					// Set destination resource if it exists in our config.
					if (resource->info->conf->rods_default_resource
							&& strlen (resource->info->conf->rods_default_resource))
						{
							addKeyVal (&open_params->condInput, DEST_RESC_NAME_KW,
									resource->info->conf->rods_default_resource);
						}

					strcpy (stream->open_params.objPath, stream->write_path);

					WHISPER("Opening write stream to <%s> for resource <%s>\n", stream->write_path, resource->uri);
					open_params->oprType = PUT_OPR;

					if (strcmp (stream->write_path, resource->info->rods_path) == 0
							&& resource->exists)
						{

							// We are overwriting an existing data object without the use of a temporary file.

							open_params->openFlags = O_WRONLY | O_CREAT;
							if (mode == DAV_MODE_WRITE_TRUNC)
								open_params->openFlags |= O_TRUNC;

							int status;

							if ((status = rcDataObjOpen (resource->info->rods_conn, open_params))
									>= 0)
								{
									openedDataObjInp_t *data_obj = &stream->data_obj;
									data_obj->l1descInx = status;
								}
							else
								{
									ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
											"rcDataObjOpen failed for <%s>: %d = %s", open_params->objPath,
											status, get_rods_error_msg (status));
									return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
											0, "Could not open destination resource for writing");
								}
						}
					else
						{
							// The iRODS C header for rcDataObjOpen tells us we can use the O_CREAT
							// flag on rcDataObjOpen, but doing so yields a CAT_NO_ROWS_FOUND
							// error.
							// We'll have to call rcDataObjCreate instead. It appears to
							// open the file in write mode, even though the docs say it
							// doesn't look at open_flags.

							WHISPER("Object does not yet exist, will create first\n");

							int status;

							if ((status = rcDataObjCreate (resource->info->rods_conn, open_params))
									>= 0)
								{
									openedDataObjInp_t *data_obj = &stream->data_obj;
									data_obj->l1descInx = status;
								}
							else
								{
									ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
											"rcDataObjCreate failed for <%s>: %d = %s", open_params->objPath,
											status, get_rods_error_msg (status));
									return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
											0, "Could not create destination resource");
								}
						}

					ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, resource->info->r,
							"Will write using %luK chunks",
							resource->info->conf->rods_tx_buffer_size / 1024);

					*result_stream = stream;

				}		/* if (stream -> write_path) */
			else
				{
					err_p = dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "No path for stream");
				}
		}
	else
		{
			err_p = dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to allocate memory for stream");
		}


	return err_p;
}

static dav_error *stream_send_buffer (dav_stream *stream, const char *buffer,
		size_t length)
{
	// Apache very reasonably passes the `repo_write_stream` function a const
	// buffer to be sent to iRODS. However, iRODS marks their rcDataObjWrite
	// input buffer as writable. This prevents us from passing the buffer on
	// directly.
	//
	// I briefly considered violating the const rule, because I strongly
	// suspect the buffer is wrongly marked non-const in iRODS code. There
	// doesn't seem to be much of a performance boost to gain from this though
	// (actually no noticable change at all when using 4M buffers). Besides
	// that, casting const away is a mortal sin.
	// So in the end we are on the safe side by copying the buffer.
	//
	// This could be slightly improved by allocating the writable buffer only
	// once (in the `stream` struct), or by making sure all writes via
	// send_buffer use the writable container, but again, the effect on
	// performance will be negligible.
	dav_error *err_p = NULL;

	stream->output_buffer.buf = (char*) malloc (length);

	if (stream->output_buffer.buf)
		{
			stream->output_buffer.len = length;

			memcpy (stream->output_buffer.buf, buffer, length);

			int written = rcDataObjWrite (stream->resource->info->rods_conn,
					&stream->data_obj, &stream->output_buffer);
			free (stream->output_buffer.buf);

			if (written < 0)
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
							stream->resource->info->r, "rcDataObjWrite failed: %d = %s", written,
							get_rods_error_msg (written));

					err_p = dav_new_error (stream->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
							"Could not write to destination resource");
				}
		}
	else
		{
			err_p = dav_new_error (stream->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Could not allocate stream output buffer");
		}

	return err_p;
}

static dav_error *stream_ship_container (dav_stream *stream)
{
	if (stream->container && stream->container_off)
		{
			dav_error *err = stream_send_buffer (stream, stream->container,
					stream->container_off);
			stream->container_off = 0;
			return err;
		}
	else
		{
			return NULL;
		}
}

static dav_error *dav_repo_write_stream (dav_stream *stream,
		const void *input_buffer, apr_size_t input_buffer_size)
{
	dav_error *err;

	// Initial testing shows that on average input buffers are around 2K in
	// size. Transferring them each to iRODS as is is incredibly inefficient.
	// That's why we collect input buffers into "containers". This way, we can
	// ship about <write_buffer_size> bytes at a time. This makes a huge
	// difference in performance (ex. from 36s to 0.8s for a 100M file when
	// switching to a 4M buffer).

	if (!stream->container)
		{
			// Initialize the container.
			stream->container_size =
					stream->resource->info->conf->rods_tx_buffer_size;
			stream->container_off = 0;

			stream->container = apr_pcalloc(stream->pool, stream->container_size);

			if (! (stream->container))
				{
					return dav_new_error (stream->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
							"Could not allocate input buffers");
				}
		}


	if (input_buffer_size >= stream->container_size)
		{
			// No need to use the container for large chunks, ship it directly
			// without wasting time copying memory.

			// First ship the current container, if any.
			err = stream_ship_container (stream);
			if (err)
				return err;
			else
				return stream_send_buffer (stream, input_buffer, input_buffer_size);
		}
	else
		{
			if (input_buffer_size > stream->container_size - stream->container_off)
				{
					// Current container's too full, ship it!
					err = stream_ship_container (stream);
					if (err)
						return err;
				}
			// input_buffer is now guaranteed to fit in our container.
			memcpy (stream->container + stream->container_off, input_buffer,
					input_buffer_size);
			stream->container_off += input_buffer_size;
			// This container will be shipped in a subsequent write, or when the
			// stream is closed.
		}

	return NULL;
}

static dav_error *dav_repo_close_stream (dav_stream *stream, int commit)
{
	// Flush the container.
	dav_error *err = stream_ship_container (stream);
	if (err)
		return err;

	const dav_resource *resource = stream->resource;

	WHISPER("Closing stream for resource <%s> / object <%s>.\n", resource->uri, stream->write_path);

	openedDataObjInp_t close_params = { 0 };
	close_params.l1descInx = stream->data_obj.l1descInx;

	int status = rcDataObjClose (resource->info->rods_conn, &close_params);
	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
					"rcDataObjClose failed: %d = %s", status,
					get_rods_error_msg (status));
			// (in the case where temp file rollback is enabled)
			// XXX: This may leave a temporary file '.davrods-*', is this okay?
			// XXX: Should we attempt to unlink the uploaded file here?
			return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
					"Could not close the uploaded resource");
		}

	if (commit)
		{
			if (strcmp (stream->write_path, resource->info->rods_path))
				{
					// We were using a temporary file, move it to the destination path.
					WHISPER(
							"Moving tempfile <%s> to destination <%s>\n",
							stream->write_path, resource->info->rods_path
					);

					// Yes, the rename function takes a copyInp struct as its input.
					dataObjCopyInp_t rename_params = { { { 0 } } };

					rename_params.srcDataObjInp.oprType = RENAME_DATA_OBJ;

					if (resource->exists)
						{
							// We are overwriting an existing data object, remove it first.

							dataObjInp_t unlink_params = { { 0 } };
							strcpy (unlink_params.objPath, resource->info->rods_path);

							// We want to bypass the trash on an upload-overwrite operation.
							addKeyVal (&unlink_params.condInput, FORCE_FLAG_KW, "");

							status = rcDataObjUnlink (resource->info->rods_conn,
									&unlink_params);

							if (status < 0)
								{
									ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
											resource->info->r, "rcDataObjUnlink failed: %d = %s",
											status, get_rods_error_msg (status));
									return dav_new_error (resource->pool,
									HTTP_INTERNAL_SERVER_ERROR, 0, 0,
											"Could not remove original file");
								}
						}

					strcpy (rename_params.srcDataObjInp.objPath, stream->write_path);
					strcpy (rename_params.destDataObjInp.objPath,
							resource->info->rods_path);

					if ((status = rcDataObjRename (resource->info->rods_conn,
							&rename_params)) < 0)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
									resource->info->r, "rcDataObjRename failed: %d = %s", status,
									get_rods_error_msg (status));

							if (status == UNIX_FILE_RENAME_ERR)
								{
									// XXX: See the iRODS issue note in dav_repo_move_resource.
									return dav_new_error (resource->pool,
									HTTP_INTERNAL_SERVER_ERROR, 0, 0,
											"iRODS Unix FS resource error: UNIX_FILE_RENAME_ERR."
													" Probably caused by a uploading a file with the name of a former collection"
													" (fs directory was not removed when iRODS collection was removed)");
								}
							else
								{
									return dav_new_error (resource->pool,
									HTTP_INTERNAL_SERVER_ERROR, 0, 0,
											"Something went wrong while renaming the uploaded resource");
								}
						}
				}
			else
				{
					// We were already writing to the destination object, so we're done here.
				}
		}
	else
		{
			// Try to perform a rollback.
			if (strcmp (stream->write_path, resource->info->rods_path))
				{
					// We were using a temporary file, simply unlink it.

					dataObjInp_t unlink_params = { { 0 } };
					strcpy (unlink_params.objPath, stream->write_path);

					// We do not want to deal with the trash when removing partially uploaded files with temporary filenames.
					addKeyVal (&unlink_params.condInput, FORCE_FLAG_KW, "");

					status = rcDataObjUnlink (resource->info->rods_conn, &unlink_params);

					if (status < 0)
						{
							ap_log_rerror (APLOG_MARK, APLOG_WARNING, APR_SUCCESS,
									resource->info->r,
									"rcDataObjUnlink of aborted upload failed: %d = %s", status,
									get_rods_error_msg (status));
							// We should not return an error here - we fulfilled the
							// client's request. There's no good way to report back this
							// error. The client needs to remove the tempfile themselves.
						}
				}
			else
				{
					if (resource->exists)
						{
							// dav_fs doesn't support rolling back writes to existing files
							// opened with SEEKABLE and neither do we.
							ap_log_rerror (APLOG_MARK, APLOG_WARNING, APR_SUCCESS,
									resource->info->r,
									"Cannot rollback write to object opened with WRITE_SEEKABLE - the original file may be trashed if writes were issued");
							// I'm not aware of a correct method to report this back to the client.

						}
					else
						{
							// This resource didn't exist yet, we can safely remove it.
							dataObjInp_t unlink_params = { { 0 } };
							strcpy (unlink_params.objPath, stream->write_path);

							// See above, we do not want to deal with the trash when removing partially uploaded files.
							addKeyVal (&unlink_params.condInput, FORCE_FLAG_KW, "");

							status = rcDataObjUnlink (resource->info->rods_conn,
									&unlink_params);

							if (status < 0)
								{
									ap_log_rerror (APLOG_MARK, APLOG_WARNING, APR_SUCCESS,
											resource->info->r,
											"rcDataObjUnlink of aborted upload failed: %d = %s",
											status, get_rods_error_msg (status));
								}
						}
				}
		}

	return NULL;
}

static dav_error *dav_repo_seek_stream (dav_stream *stream, apr_off_t abs_pos)
{
	openedDataObjInp_t seek_inp = { 0 };
	seek_inp.l1descInx = stream->data_obj.l1descInx;
	seek_inp.offset = abs_pos;
	seek_inp.whence = SEEK_SET;

	fileLseekOut_t *seek_out = NULL;
	int status = rcDataObjLseek (stream->resource->info->rods_conn, &seek_inp,
			&seek_out);

	if (seek_out)
		free (seek_out);

	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
					stream->resource->info->r, "rcDataObjLseek failed: %d = %s", status,
					get_rods_error_msg (status));
			return dav_new_error (stream->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
					"Could not seek file for partial upload or resume");
		}
	else
		{
			return NULL;
		}
}

static const char *dav_repo_getetag (const dav_resource *resource);

static dav_error *dav_repo_set_headers (request_rec *r,
		const dav_resource *resource)
{
	// Set response headers for GET requests.

	if (resource->collection)
		{
			// A GET on a collection => client must be a web browser / standard HTTP client.
			// We will output an HTML directory listing.
			ap_set_content_type (r, "text/html; charset=utf-8");
			// Do not let them cache directory content renders.
			apr_table_setn (r->headers_out, "Cache-Control",
					"no-cache, must-revalidate");
		}
	else
		{
			const char *etag = dav_repo_getetag (resource);
			char *date_str = apr_pcalloc(r->pool, APR_RFC822_DATE_LEN);

			if (date_str)
				{
					uint64_t timestamp = atoll (resource->info->stat->modifyTime);
					int status = apr_rfc822_date (date_str, timestamp * 1000 * 1000);

					apr_table_setn (r->headers_out, "Last-Modified",
							status >= 0 ? date_str : "Thu, 01 Jan 1970 00:00:00 GMT");
				}		/* if (date_str) */

			if (etag && strlen (etag))
				{
					apr_table_setn (r->headers_out, "ETag", etag);
				}

			ap_set_content_length (r, resource->info->stat->objSize);
		}

	return 0;
}

static dav_error *deliver_file (const dav_resource *resource,
		ap_filter_t *output)
{
	// Download a file from iRODS to the WebDAV client.

	apr_pool_t *pool = resource->pool;
	apr_bucket_brigade *bb;
	apr_bucket *bkt;

	bb = apr_brigade_create (pool, output->c->bucket_alloc);

	dataObjInp_t open_params = { { 0 } };

	open_params.openFlags = O_RDONLY;
	strcpy (open_params.objPath, resource->info->rods_path);

	int status;

	if ((status = rcDataObjOpen (resource->info->rods_conn, &open_params)) < 0)
		{
			apr_brigade_destroy (bb);

			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
					"rcDataObjOpen failed: %d = %s", status, get_rods_error_msg (status));

			// Note: This might be a CONFLICT situation where the file was deleted
			//       in a separate concurrent request.

			return dav_new_error (pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
					"Could not open requested resource for reading");
		}
	else
		{
			// `status` contains some sort of file descriptor.
			openedDataObjInp_t data_obj = { .l1descInx = status };

			bytesBuf_t read_buffer = { 0 };

			size_t buffer_size = resource->info->conf->rods_rx_buffer_size;
			data_obj.len = buffer_size;

			int bytes_read = 0;

			ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, resource->info->r,
					"Reading data object in %luK chunks", buffer_size / 1024);

			// Read from iRODS, write to the client.
			do
				{
					bytes_read = rcDataObjRead (resource->info->rods_conn, &data_obj,
							&read_buffer);

					if (bytes_read < 0)
						{
							if (read_buffer.buf)
								free (read_buffer.buf);
							apr_brigade_destroy (bb);

							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
									resource->info->r, "rcDataObjRead failed: %d = %s",
									bytes_read, get_rods_error_msg (bytes_read));
							return dav_new_error (pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
									"Could not read from requested resource");
						}
					apr_brigade_write (bb, NULL, NULL, read_buffer.buf, bytes_read);

					free (read_buffer.buf);
					read_buffer.buf = NULL;

					if ((status = ap_pass_brigade (output, bb)) != APR_SUCCESS)
						{
							apr_brigade_destroy (bb);
							return dav_new_error (pool, HTTP_INTERNAL_SERVER_ERROR, 0, status,
									"Could not write contents to filter.");
						}
				}
			while ((size_t) bytes_read == buffer_size);

			openedDataObjInp_t close_params = { 0 };
			close_params.l1descInx = data_obj.l1descInx;

			status = rcDataObjClose (resource->info->rods_conn, &close_params);
			if (status < 0)
				{
					ap_log_rerror (APLOG_MARK, APLOG_WARNING, APR_SUCCESS,
							resource->info->r,
							"rcDataObjClose failed: %d = %s (proceeding as if nothing happened)",
							status, get_rods_error_msg (status));
					// We already gave the entire file to the client, it makes no sense to send them an error here.

					//return dav_new_error(
					//    pool, HTTP_INTERNAL_SERVER_ERROR, 0, status,
					//    "Could not close requested data object"
					//);
				}

		}

	bkt = apr_bucket_eos_create (output->c->bucket_alloc);

	APR_BRIGADE_INSERT_TAIL(bb, bkt);

	if ((status = ap_pass_brigade (output, bb)) != APR_SUCCESS)
		{
			apr_brigade_destroy (bb);
			return dav_new_error (pool, HTTP_INTERNAL_SERVER_ERROR, 0, status,
					"Could not write contents to filter.");
		}
	apr_brigade_destroy (bb);

	return NULL;
}

static dav_error *deliver_directory (const dav_resource *resource,
		ap_filter_t *output)
{
	// Print a basic HTML directory listing.
	collInp_t coll_inp = { { 0 } };
	strcpy (coll_inp.collName, resource->info->rods_path);

	collHandle_t coll_handle = { 0 };

	// Open the collection.
	collEnt_t coll_entry;
	int status = rclOpenCollection (resource->info->rods_conn,
			resource->info->rods_path, LONG_METADATA_FG, &coll_handle);

	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
					"rcOpenCollection failed: %d = %s", status,
					get_rods_error_msg (status));

			return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
					status, "Could not open a collection");
		}

	// Make brigade.
	apr_pool_t *pool = resource->pool;
	apr_bucket_brigade *bb = apr_brigade_create (pool, output->c->bucket_alloc);

	// Send start of HTML document.
	apr_brigade_printf (bb, NULL, NULL,
			"<!DOCTYPE html>\n<html>\n<head><title>Index of %s on %s</title></head>\n",
			ap_escape_html(pool, resource->info->relative_uri),
			ap_escape_html(pool, resource->info->conf->rods_zone));
	apr_brigade_printf (bb, NULL, NULL,
			"<body>\n\n"
					"<!-- Warning: Do not parse this directory listing programmatically,\n"
					"              the format may change without notice!\n"
					"              If you want to script access to these WebDAV collections,\n"
					"              please use the PROPFIND method instead. -->\n\n"
					"<h1>Index of %s on %s</h1>\n",
			ap_escape_html(pool, resource->info->relative_uri),
			ap_escape_html(pool, resource->info->conf->rods_zone));

	if (strcmp (resource->info->relative_uri, "/"))
		apr_brigade_puts (bb, NULL, NULL,
				"<p><a href=\"..\">↖ Parent collection</a></p>\n");

	apr_brigade_puts (bb, NULL, NULL,
			"<table>\n<thead>\n"
					"  <tr><th>Name</th><th>Size</th><th>Owner</th><th>Last modified</th></tr>\n"
					"</thead>\n<tbody>\n");

	// Actually print the directory listing, one table row at a time.
	do
		{
			status = rclReadCollection (resource->info->rods_conn, &coll_handle,
					&coll_entry);

			if (status < 0)
				{
					if (status == CAT_NO_ROWS_FOUND)
						{
							// End of collection.
						}
					else
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
									resource->info->r,
									"rcReadCollection failed for collection <%s> with error <%s>",
									resource->info->rods_path, get_rods_error_msg (status));

							apr_brigade_destroy (bb);

							return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR,
									0, 0, "Could not read a collection entry from a collection.");
						}
				}
			else
				{
					apr_brigade_puts (bb, NULL, NULL, "  <tr>");

					const char *name =
							coll_entry.objType == DATA_OBJ_T ?
									coll_entry.dataName : get_basename (coll_entry.collName);

					// Generate link.
					if (coll_entry.objType == COLL_OBJ_T)
						{
							// Collection links need a trailing slash for the '..' links to work correctly.
							apr_brigade_printf (bb, NULL, NULL,
									"<td><a href=\"%s/\">%s/</a></td>",
									ap_escape_html(pool, ap_escape_uri(pool, name)),
									ap_escape_html(pool, name));
						}
					else
						{
							apr_brigade_printf (bb, NULL, NULL,
									"<td><a href=\"%s\">%s</a></td>",
									ap_escape_html(pool, ap_escape_uri(pool, name)),
									ap_escape_html(pool, name));
						}

					// Print data object size.
					if (coll_entry.objType == DATA_OBJ_T)
						{
							char size_buf [5] = { 0 };
							// Fancy file size formatting.
							apr_strfsize (coll_entry.dataSize, size_buf);
							if (size_buf [0])
								apr_brigade_printf (bb, NULL, NULL, "<td>%s</td>", size_buf);
							else
								apr_brigade_printf (bb, NULL, NULL, "<td>%lu</td>",
										coll_entry.dataSize);
						}
					else
						{
							apr_brigade_puts (bb, NULL, NULL, "<td></td>");
						}

					// Print owner.
					apr_brigade_printf (bb, NULL, NULL, "<td>%s</td>",
							ap_escape_html(pool, coll_entry.ownerName));

					// Print modified-date string.
					uint64_t timestamp = atoll (coll_entry.modifyTime);
					apr_time_t apr_time = 0;
					apr_time_exp_t exploded = { 0 };
					char date_str [64] = { 0 };

					apr_time_ansi_put (&apr_time, timestamp);
					apr_time_exp_lt (&exploded, apr_time);

					size_t ret_size;
					if (!apr_strftime (date_str, &ret_size, sizeof(date_str),
							"%Y-%m-%d %H:%M", &exploded))
						{
							apr_brigade_printf (bb, NULL, NULL, "<td>%s</td>",
									ap_escape_html(pool, date_str));
						}
					else
						{
							// Fallback, just in case.
							static_assert (sizeof(date_str) >= APR_RFC822_DATE_LEN,
									"Size of date_str buffer too low for RFC822 date");

							status = apr_rfc822_date (date_str, timestamp * 1000 * 1000);

							apr_brigade_printf (bb, NULL, NULL, "<td>%s</td>",
									ap_escape_html(pool,
											status >= 0 ?
													date_str : "Thu, 01 Jan 1970 00:00:00 GMT"));
						}

					apr_brigade_puts (bb, NULL, NULL, "</tr>\n");
				}
		}
	while (status >= 0);

	// End HTML document.
	apr_brigade_puts (bb, NULL, NULL, "</tbody>\n</table>\n</body>\n</html>\n");

	// Flush.
	if ((status = ap_pass_brigade (output, bb)) != APR_SUCCESS)
		{
			apr_brigade_destroy (bb);
			return dav_new_error (pool, HTTP_INTERNAL_SERVER_ERROR, 0, status,
					"Could not write contents to filter.");
		}

	CloseBucketsStream (bb);

	if ((status = ap_pass_brigade (output, bb)) != APR_SUCCESS)
		{
			apr_brigade_destroy (bb);
			return dav_new_error (pool, HTTP_INTERNAL_SERVER_ERROR, 0, status,
					"Could not write content to filter.");
		}
	apr_brigade_destroy (bb);

	return NULL;
}

static dav_error *dav_repo_deliver (const dav_resource *resource,
		ap_filter_t *output)
{
	// Deliver response body for GET requests.

	if (resource->type != DAV_RESOURCE_TYPE_REGULAR
			&& resource->type != DAV_RESOURCE_TYPE_VERSION
			&& resource->type != DAV_RESOURCE_TYPE_WORKING)
		{
			return dav_new_error (resource->pool, HTTP_CONFLICT, 0, 0,
					"Cannot GET this type of resource.");
		}

	if (resource->collection)
		{
			davrods_dir_conf_t *conf_p = resource->info->conf;

			if (conf_p -> themed_listings)
				{
					return DeliverThemedDirectory (resource, output);
				}
			else
				{
					return deliver_directory (resource, output);
				}
		}
	else
		{
			return deliver_file (resource, output);
		}
}

static dav_error *dav_repo_create_collection (dav_resource *resource)
{
	WHISPER("Creating collection at <%s> = <%s>\n", resource->uri, resource->info->rods_path);

	dav_resource *parent;
	dav_error *err = dav_repo_get_parent_resource (resource, &parent);
	if (err)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
					"Getting parent resource of <%s> failed in create_collection()",
					resource->uri);
			return err;
		}

	if (!parent->exists)
		{
			return dav_new_error (resource->pool, HTTP_CONFLICT, 0, 0,
					"Parent directory does not exist.");
		}

	collInp_t coll_inp = { { 0 } };
	strcpy (coll_inp.collName, resource->info->rods_path);

	int status = rcCollCreate (resource->info->rods_conn, &coll_inp);
	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
					"rcCollCreate failed: %d = %s", status, get_rods_error_msg (status));

			return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
					status, "Could not create a collection at the given path");
		}

	// Update resource stat info.
	return get_dav_resource_rods_info (resource);;
}



static bool walker_have_seen_path (const walker_seen_resource_t *seen,
		const char *rods_path)
{
	for (; seen; seen = seen->next)
		{
			if (strcmp (seen->rods_path, rods_path) == 0)
				return true;
		}
	return false;
}

static int walker_push_seen_path (apr_pool_t *p, walker_seen_resource_t **seen, const char *rods_path)
{
	int res = 1;
	walker_seen_resource_t *current;
	current = apr_pcalloc(p, sizeof(walker_seen_resource_t));

	if (current)
		{
			current->rods_path = apr_pstrdup (p, rods_path);

			if (current -> rods_path)
				{
					if (*seen)
						{
							// Append.
							walker_seen_resource_t *tail = *seen;
							while (tail->next)
								{
									tail = tail->next;
								}

							tail->next = current;
						}
					else
						{
							*seen = current;
						}

				}		/* if (current -> rods_path) */


		}		/* if (current) */

	return res;
}


static dav_error *walker (struct dav_repo_walker_private *ctx, int depth)
{
	WHISPER("Entered walker (%d/%s), depth is %d - Current object <%s> is a %s.\n",
			ctx->params->walk_type,
			ctx->params->walk_type == DAV_WALKTYPE_AUTH
			? "AUTH"
			: ctx->params->walk_type == DAV_WALKTYPE_NORMAL
			? "NORMAL"
			: ctx->params->walk_type == DAV_WALKTYPE_LOCKNULL
			? "LOCKNULL"
			: ctx->params->walk_type & DAV_WALKTYPE_NORMAL
			? "NORMAL+"
			: "?",
			depth,
			ctx->resource.info->rods_path,
			ctx->resource.collection
			? "collection"
			: "data object"
	);WHISPER("Exists(%c)\n", ctx->resource.exists?'T':'F');

	WHISPER("Calling walker callback for object uri <%s>\n", ctx->resource.uri);
	dav_error *err = (*ctx->params->func) (&ctx->wres,
			ctx->resource.collection ? DAV_CALLTYPE_COLLECTION : DAV_CALLTYPE_MEMBER);

	if (err)
		{
			WHISPER("Walker callback returned an error, aborting. description: %s", err->desc);
			return err;
		}

	if (depth == 0 || !ctx->resource.collection)
		{
			WHISPER("Reached end of recurse (depth:%d, collection:%d)\n", depth, ctx->resource.collection);
			return NULL;
		}

	collHandle_t coll_handle;
	collEnt_t coll_entry;

	WHISPER("Opening iRODS collection <%s> \n", ctx->resource.info->rods_path);

	int status = rclOpenCollection (ctx->resource.info->rods_conn,
			ctx->resource.info->rods_path, 0, &coll_handle);
	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, ctx->resource.info->r,
					"rcOpenCollection failed: %d = %s", status,
					get_rods_error_msg (status));

			return dav_new_error (ctx->resource.pool, HTTP_INTERNAL_SERVER_ERROR, 0,
					status, "Could not open a collection");
		}

	size_t rods_path_len = strlen (ctx->resource.info->rods_path);
	size_t uri_len = strlen (ctx->uri_buffer);

	// Keep track of seen child resources. We will need this to filter
	// out existing resource if a LOCKNULL walk was requested.
	walker_seen_resource_t *seen_resource = NULL;

	WHISPER("Entering read loop of iRODS collection <%s>\n", ctx->resource.info->rods_path);

	do
		{
			status = rclReadCollection (ctx->resource.info->rods_conn, &coll_handle,
					&coll_entry);

			if (status < 0)
				{
					if (status == CAT_NO_ROWS_FOUND)
						{
							WHISPER("Reached end of collection <%s>.\n", ctx->resource.info->rods_path);
						}
					else
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
									ctx->resource.info->r,
									"rcReadCollection failed for collection <%s> with error <%s>",
									ctx->resource.info->rods_path, get_rods_error_msg (status));
							// XXX: Perhaps report CONFLICT instead of depending on `status`?
							//      How do clients handle this?
							return dav_new_error (ctx->resource.pool,
							HTTP_INTERNAL_SERVER_ERROR, 0, 0,
									"Could not read a collection entry from a collection.");
						}
				}
			else
				{
					const char *name =
							coll_entry.objType == DATA_OBJ_T ?
									coll_entry.dataName : get_basename (coll_entry.collName);

					WHISPER("Got a collection entry: %s '%s', %" DAVRODS_SIZE_T_FMT " bytes\n",
							coll_entry.objType == DATA_OBJ_T
							? "Data object"
							: coll_entry.objType == COLL_OBJ_T
							? "Collection"
							: "Thing",
							name,
							coll_entry.dataSize
					);

					if (uri_len + 1 + strlen (name) >= MAX_NAME_LEN
							|| rods_path_len + 1 + strlen (name) >= MAX_NAME_LEN)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
									ctx->resource.info->r,
									"Generated an uri or iRODS path exceeding iRODS path length limits");
							return dav_new_error (ctx->resource.pool,
							HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Path name too long");
						}

					// Transform resource struct into child resource struct.
					// Perform the same path translation on both rods_path and uri.

					if (strcmp (ctx->uri_buffer, "/") == 0)
						{
							strcat (ctx->uri_buffer, name);
						}
					else
						{
							ctx->uri_buffer [uri_len] = '/';
							strcpy (ctx->uri_buffer + uri_len + 1, name);
						}
					if (strcmp (ctx->resource.info->rods_path, "/") == 0)
						{
							strcat (ctx->resource.info->rods_path, name);
						}
					else
						{
							ctx->resource.info->rods_path [rods_path_len] = '/';
							strcpy (ctx->resource.info->rods_path + rods_path_len + 1, name);
						}

					ctx->resource.exists = 1;
					ctx->resource.collection = (coll_entry.objType == COLL_OBJ_T);

					if (ctx->resource.info->stat)
						{
							ctx->resource.info->stat->objSize =
									ctx->resource.collection ? 0 : coll_entry.dataSize;
							strncpy (ctx->resource.info->stat->modifyTime, coll_entry.modifyTime,
									sizeof(ctx->resource.info->stat->modifyTime));
							strncpy (ctx->resource.info->stat->createTime, coll_entry.createTime,
									sizeof(ctx->resource.info->stat->createTime));

							if (!walker_push_seen_path (ctx->resource.pool, &seen_resource, ctx->resource.info->rods_path))
								{
									ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_EGENERAL, ctx->resource.info->r, "Failed to walk \"%s\"", seen_resource -> rods_path);

									return dav_new_error (ctx->resource.pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to walk path");
								}		/* if (!walker_push_seen_path (ctx->resource.pool, &seen_resource, ctx->resource.info->rods_path)) */

							walker (ctx, depth - 1);

							// Reset resource paths to original.
							ctx->uri_buffer [uri_len] = '\0';
							ctx->resource.info->rods_path [rods_path_len] = '\0';
						}
					else
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_EGENERAL, ctx->resource.info->r, "Failed to stat \"%s\"", ctx->resource.info -> rods_path);

							return dav_new_error (ctx->resource.pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to stat");
						}
				}

		}
	while (status >= 0);

	if (ctx->params->walk_type & DAV_WALKTYPE_LOCKNULL)
		{
			// A LOCKNULL walk must call the callback function for
			// locknull members (that is, member resources that don't
			// exist, but have been locked in advance).

#ifdef DAVRODS_ENABLE_PROVIDER_LOCALLOCK
			// We can only support LOCKNULL walks using our own locking
			// provider, locallock. The generic locking provider
			// mod_dav_lock seems to miss an interface for this
			// functionality.
			// I would love to simply depend on mod_dav_lock instead of
			// forking it just for Davrods, but this issue prevents that.
			//
			// There's also the issue that mod_dav_lock locks by URI. We
			// cannot use that since the same URI may lead to different
			// resources for different users, depending on the
			// DavrodsExposedRoot setting.
			extern const dav_provider davrods_dav_provider_locallock;

			dav_lockdb *db = ctx->params->lockdb;
			assert(db); // This would be a mod_dav logic bug.

			if (ctx->params->lockdb->hooks == davrods_dav_provider_locallock.locks)
				{
					WHISPER("Checking locks for <%s>", ctx->resource.uri);

					davrods_locklocal_lock_list_t *locked_name;
					err = davrods_locklocal_get_locked_entries (db, &ctx->resource,
							&locked_name);
					if (err)
						return err;

					for (; locked_name; locked_name = locked_name->next)
						{
							if (walker_have_seen_path (seen_resource, locked_name->entry))
								{
									continue;
								}

							const char *name = get_basename (locked_name->entry);

							if (uri_len + 1 + strlen (name) >= MAX_NAME_LEN
									|| rods_path_len + 1 + strlen (name) >= MAX_NAME_LEN)
								{
									ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
											ctx->resource.info->r,
											"Generated an uri or iRODS path exceeding iRODS path length limits");
									return dav_new_error (ctx->resource.pool,
									HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Path name too long");
								}
							if (strcmp (ctx->uri_buffer, "/") == 0)
								{
									strcat (ctx->uri_buffer, name);
								}
							else
								{
									ctx->uri_buffer [uri_len] = '/';
									strcpy (ctx->uri_buffer + uri_len + 1, name);
								}
							if (strcmp (ctx->resource.info->rods_path, "/") == 0)
								{
									strcat (ctx->resource.info->rods_path, name);
								}
							else
								{
									ctx->resource.info->rods_path [rods_path_len] = '/';
									strcpy (ctx->resource.info->rods_path + rods_path_len + 1,
											name);
								}

							ctx->resource.exists = 0;
							ctx->resource.collection = 0;

							// Call callback function.
							err = (*ctx->params->func) (&ctx->wres, DAV_CALLTYPE_LOCKNULL);

							// Reset resource paths to original.
							ctx->uri_buffer [uri_len] = '\0';
							ctx->resource.info->rods_path [rods_path_len] = '\0';

							if (err)
								{
									WHISPER("(LOCKNULL) Walker callback returned an error, aborting. description: %s", err->desc);
									return err;
								}

						}

#else 
					// Can we support other locking providers' LOCKNULL walking
					// functionality? (there are no other locking providers as far
					// as I know).
					if (false)
						{
#endif /* DAVRODS_ENABLE_PROVIDER_LOCALLOCK */

				}
			else
				{
					WHISPER("LOCKNULL walk requested, but we can't provide it.");
				}
		}WHISPER("walker function end\n");
	return NULL;
}

static dav_error *dav_repo_walk (const dav_walk_params *params, int depth,
		dav_response **response)
{
	dav_error *err_p = NULL;
	struct dav_repo_walker_private ctx = { 0 };

	ctx.params = params;

	struct dav_resource_private *ctx_res_private = apr_pcalloc(params->root->pool, sizeof(*ctx_res_private));

	if (ctx_res_private)
		{
			copy_resource_context (ctx_res_private, ctx.params->root->info);

			// FIXME: Use pool provided by walker params.
			ctx_res_private->stat = apr_pcalloc(params->root->pool,
					sizeof(rodsObjStat_t));
			WHISPER("Private @ %p\n", ctx_res_private);WHISPER("root info @ %p\n", ctx.params->root->info);WHISPER("root stat @ %p\n", ctx.params->root->info->stat);

			if (ctx_res_private -> stat)
				{
					// LockNull related walks can encounter non-existant resources.
					// Stat will be NULL for such resources.
					if (ctx.params->root->info->stat)
						*ctx_res_private->stat = *ctx.params->root->info->stat;

					// We need to use a writable URI buffer in ctx because dav_resource's uri
					// property is const.
					if (strlen (ctx_res_private->r->uri) >= MAX_NAME_LEN)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS,
									ctx.params->root->info->r,
									"URI length exceeds walker's URI buffer size (%u bytes)",
									sizeof(ctx.uri_buffer));
							return dav_new_error (ctx.params->root->pool, HTTP_INTERNAL_SERVER_ERROR,
									0, 0, "Request URI too long");
						}
					strcpy (ctx.uri_buffer, ctx.params->root->uri);

					ctx.resource.exists = ctx.params->root->exists;
					ctx.resource.collection = ctx.params->root->collection;

					// Point the resource URI to our uri buffer.
					ctx.resource.uri = ctx.uri_buffer;

					ctx.resource.type = DAV_RESOURCE_TYPE_REGULAR;
					ctx.resource.hooks = &davrods_hooks_repository;
					ctx.resource.pool = ctx_res_private->r->pool;
					ctx.resource.info = ctx_res_private;

					err_p = set_rods_path_from_uri (&ctx.resource);
					if (!err_p)
						{
							ctx.wres.walk_ctx = params->walk_ctx;
							ctx.wres.pool = params->pool;
							ctx.wres.resource = &ctx.resource;

							err_p = walker (&ctx, depth);

							*response = ctx.wres.response;
						}
				}		/* if (ctx_res_private -> stat) */
			else
				{
					err_p = dav_new_error (params -> root -> pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to copy resource stat");
				}

		}		/* if (ctx_res_private) */
	else
		{
			err_p = dav_new_error (params -> root -> pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to copy resource");
		}

	return err_p;
}


typedef struct
{
	const char *src_rods_root;
	const char *dst_rods_root;
} dav_copy_walk_private;

static dav_error *dav_copy_walk_callback (dav_walk_resource *wres, int calltype)
{
	dav_error *err_p = NULL;
	const dav_resource *resource = wres->resource;
	dav_copy_walk_private *ctx = (dav_copy_walk_private*) wres->walk_ctx;

	if (ctx)
		{
			size_t src_root_len = strlen (ctx->src_rods_root);
			size_t dst_root_len = strlen (ctx->dst_rods_root);

			const char *src_path = resource->info->rods_path;
			char dst_path [MAX_NAME_LEN];

			WHISPER("COPY: At resource <%s> srcroot<%s>, dstroot<%s>\n", resource->uri, ctx->src_rods_root, ctx->dst_rods_root);


			strcpy (dst_path, ctx->dst_rods_root);

			size_t src_len = strlen (src_path);

			if (src_len > src_root_len)
				{
					if (dst_root_len + (src_len - src_root_len) < MAX_NAME_LEN)
						{
							strcat (dst_path, src_path + src_root_len);
						}
					else
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
									"Generated a copy destination filename exceeding iRODS MAX_NAME_LEN (%u) limits for source resource <%s> (%lu+(%lu-%lu)). Aborting copy.",
									MAX_NAME_LEN, resource->uri, dst_root_len, src_root_len, src_len);
							return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
									0, "A destination path exceeds the maximum filename length.");
						}
				}

			WHISPER("COPY: current dest <%s>\n", dst_path);

			if (resource->collection)
				{
					// Create collection.
					collInp_t mkdir_params = { { 0 } };
					strcpy (mkdir_params.collName, dst_path);

					int status = rcCollCreate (resource->info->rods_conn, &mkdir_params);
					if (status < 0)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
									"rcCollCreate failed: %d = %s", status,
									get_rods_error_msg (status));
							return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
									0, "Could not create collection.");
						}
				}
			else
				{
					// Copy data object.
					dataObjCopyInp_t copy_params = { { { 0 } } };

					// Set destination resource if it exists in our config.
					if (resource->info->conf->rods_default_resource
							&& strlen (resource->info->conf->rods_default_resource))
						{
							addKeyVal (&copy_params.destDataObjInp.condInput, DEST_RESC_NAME_KW,
									resource->info->conf->rods_default_resource);
						}

					dataObjInp_t *obj_src = &copy_params.srcDataObjInp;
					dataObjInp_t *obj_dst = &copy_params.destDataObjInp;

					strcpy (obj_src->objPath, src_path);
					strcpy (obj_dst->objPath, dst_path);

					addKeyVal (&obj_dst->condInput, FORCE_FLAG_KW, "");

					int status = rcDataObjCopy (resource->info->rods_conn, &copy_params);
					if (status < 0)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
									"rcDataObjCopy failed: %d = %s", status,
									get_rods_error_msg (status));
							return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
									0, "Could not copy file.");
						}
				}

		}
	else
		{
			err_p =  dav_new_error (resource -> pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, "Failed to get dav copy data");
		}

	return err_p;
}

static dav_error *dav_repo_copy_resource (const dav_resource *src,
		dav_resource *dst, int depth, dav_response **response)
{
	WHISPER("Copying resource <%s> to <%s>, depth %d\n", src->uri, dst->uri, depth);

	dav_resource *dst_parent;

	dav_error *err = dav_repo_get_parent_resource (dst, &dst_parent);
	if (err)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, src->info->r,
					"Getting parent resource of <%s> failed in copy_resource()",
					dst->uri);
			return err;
		}

	if (!dst_parent->exists)
		{
			return dav_new_error (dst->pool, HTTP_CONFLICT, 0, 0,
					"Parent directory does not exist.");
		}

	dav_copy_walk_private copy_ctx = { 0 };
	copy_ctx.src_rods_root = src->info->rods_path;
	copy_ctx.dst_rods_root = dst->info->rods_path;

	dav_walk_params walk_params = { .walk_type = DAV_WALKTYPE_NORMAL, .func =
			dav_copy_walk_callback, .walk_ctx = &copy_ctx, .pool = src->pool, .root =
			src, .lockdb = NULL };

	err = dav_repo_walk (&walk_params, depth, response);

	return err;
}

static dav_error *dav_repo_move_resource (dav_resource *src, dav_resource *dst,
		dav_response **response)
{
	WHISPER("Moving resource <%s> to <%s>\n", src->uri, dst->uri);

	// Yes, the rename function takes a copyInp struct as its input.
	dataObjCopyInp_t rename_params = { { { 0 } } };

	rename_params.srcDataObjInp.oprType =
			src->collection ? RENAME_COLL : RENAME_DATA_OBJ;

	strcpy (rename_params.srcDataObjInp.objPath, src->info->rods_path);
	strcpy (rename_params.destDataObjInp.objPath, dst->info->rods_path);

	int status = rcDataObjRename (src->info->rods_conn, &rename_params);
	if (status < 0)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, src->info->r,
					"rcDataObjRename failed: %d = %s", status,
					get_rods_error_msg (status));

			if (status == UNIX_FILE_RENAME_ERR)
				{
					// XXX iRODS issue in 3.3, 4.1, and possibly later versions:
					//     On rmcol / irm -r, unix filesystem directories in the vault
					//     are not removed. When trying to rename a data object to the
					//     name of an old, removed collection, the UNIX filesystem
					//     resource fails with the following error:

					/*
					 Jan  4 17:21:12 pid:15626 ERROR: [-]    iRODS/server/api/src/rsFileRename.cpp:115:_rsFileRename :  status [UNIX_FILE_RENAME_ERR]  errno [Is a directory] -- message [fileRename failed for [/var/lib/irods/iRODS/Vault/home/chris/litmus/.davrods-tx-3a8e-568aaa08] to [/var/lib/irods/iRODS/Vault/home/chris/litmus/mvnoncoll]]
					 [-]     iRODS/server/drivers/src/fileDriver.cpp:474:fileRename :  status [UNIX_FILE_RENAME_ERR]  errno [Is a directory] -- message [failed to call 'rename']
					 [-]     libunixfilesystem.cpp:1106:unix_file_rename_plugin :  status [UNIX_FILE_RENAME_ERR]  errno [Is a directory] -- message [Rename error for "/var/lib/irods/iRODS/Vault/home/chris/litmus/.davrods-tx-3a8e-568aaa08" to "/var/lib/irods/iRODS/Vault/home/chris/litmus/mvnoncoll", errno = "Is a directory", status = -528021.]
					 */

					return dav_new_error (src->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
							"iRODS Unix FS resource error: UNIX_FILE_RENAME_ERR."
									" Probably caused by a renaming a file to the name of a former collection"
									" (fs directory was not removed when iRODS collection was removed)");
				}
			else
				{
					return dav_new_error (src->pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0,
							"Something went wrong while renaming a resource");
				}
		}

	src->exists = 0;
	dst->exists = 1;
	dst->collection = src->collection;

	return 0;
}

static dav_error *dav_repo_remove_resource (dav_resource *resource,
		dav_response **response)
{
	dav_error *err_p = NULL;

 if (resource->exists)
	 {
			request_rec *r = resource->info->r;

			if (resource->collection)
				{
					ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r,
							"Removing collection <%s>", resource->info->rods_path);

					collInp_t rmcoll_params = { { 0 } };

					// `rods_path` is guaranteed not to exceed MAX_NAME_LEN.
					strcpy (rmcoll_params.collName, resource->info->rods_path);

					// Do we remove recursively? Yes.
					addKeyVal (&rmcoll_params.condInput, RECURSIVE_OPR__KW, "");
					// Uncomment for trash bypass.
					//addKeyVal(&rmcoll_params.condInput, FORCE_FLAG_KW, "");

					int status = rcRmColl (resource->info->rods_conn, &rmcoll_params, 0);
					if (status < 0)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
									"rcRmColl failed: %d = %s", status, get_rods_error_msg (status));
							return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
									0, "Could not remove collection.");
						}

					resource->exists = 0;
					resource->collection = 0;
				}
			else
				{
					ap_log_rerror (APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r,
							"Removing data object <%s>", resource->info->rods_path);

					dataObjInp_t unlink_params = { { 0 } };
					strcpy (unlink_params.objPath, resource->info->rods_path);

					// Uncomment for trash bypass.
					//addKeyVal(&unlink_params.condInput, FORCE_FLAG_KW, "");

					int status = rcDataObjUnlink (resource->info->rods_conn, &unlink_params);
					if (status < 0)
						{
							ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
									"rcDataObjUnlink failed: %d = %s", status,
									get_rods_error_msg (status));
							return dav_new_error (resource->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
									0, "Could not remove file.");
						}

					resource->exists = 0;
				}

	 }
 else
	 {
		 char *error_s = apr_pstrcat (resource -> pool, "resource \"", resource -> uri, "\" does not exist", NULL);
		 err_p = dav_new_error (resource -> pool, HTTP_INTERNAL_SERVER_ERROR, 0, 0, error_s);
	 }


	return err_p;
}

static const char *dav_repo_getetag (const dav_resource *resource)
{
	// This mimicks dav_fs repo's getetag.

	dav_resource_private *res_private = resource->info;

	if (!resource->exists)
		{
			return "";

		}
	else if (resource->collection)
		{
			return apr_psprintf (resource->pool, "\"%s\"",
					res_private->stat->modifyTime);
		}
	else
		{
			return apr_psprintf (resource->pool, "\"%" APR_UINT64_T_HEX_FMT "-%s\"",
					(apr_uint64_t) res_private->stat->objSize,
					res_private->stat->modifyTime);
		}
}

static request_rec *dav_repo_get_request_rec (const dav_resource *resource)
{
	return resource->info->r;
}

const char *dav_repo_pathname (const dav_resource *resource)
{
	// XXX: This function is never called by mod_dav. Apparently it is only
	// used within mod_dav_fs (unrelated to this module), so there is no
	// pressing need to implement it.
	ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, resource->info->r,
			"Unimplemented Davrods function <%s>", __func__);

	// TODO.

	return NULL;
}

const dav_hooks_repository davrods_hooks_repository = {
		1, // handle_get, we will.
		dav_repo_get_resource, dav_repo_get_parent_resource,
		dav_repo_is_same_resource, dav_repo_is_parent_resource,
		dav_repo_open_stream, dav_repo_close_stream, dav_repo_write_stream,
		dav_repo_seek_stream, dav_repo_set_headers, dav_repo_deliver,
		dav_repo_create_collection, dav_repo_copy_resource, dav_repo_move_resource,
		dav_repo_remove_resource, dav_repo_walk, dav_repo_getetag,
		NULL, dav_repo_get_request_rec, dav_repo_pathname /* Unimplemented: see comment in that function */
};
