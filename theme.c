/*
 * theme.c
 *
 *  Created on: 28 Sep 2016
 *      Author: billy
 */

#include "theme.h"
#include "meta.h"
#include "repo.h"

#include "config.h"

/************************************/

static void PrintMetadata (apr_bucket_brigade *bb_p, IrodsMetadata **metadata_pp, int size);

static int CompareIrodsMetadata (const void *v0_p, const void *v1_p);

/*************************************/


void InitHtmlTheme (struct HtmlTheme *theme_p)
{
  theme_p -> ht_head_s = NULL;
  theme_p -> ht_top_s = NULL;
  theme_p -> ht_bottom_s = NULL;
  theme_p -> ht_collection_icon_s = NULL;
  theme_p -> ht_object_icon_s = NULL;
  theme_p -> ht_parent_icon_s = NULL;
  theme_p -> ht_listing_class_s = NULL;
  theme_p -> ht_show_metadata = 0;
}


dav_error *DeliverThemedDirectory (const dav_resource *resource_p, ap_filter_t *output_p)
{
	struct dav_resource_private *davrods_resource_p = (struct dav_resource_private *) resource_p -> info;
	apr_pool_t *pool_p = resource_p -> pool;

	collInp_t coll_inp = { { 0 } };
	collHandle_t coll_handle = { 0 };
	collEnt_t coll_entry;
	int status;

	strcpy(coll_inp.collName, davrods_resource_p->rods_path);

	// Open the collection
	status = rclOpenCollection (davrods_resource_p->rods_conn, davrods_resource_p->rods_path, LONG_METADATA_FG, &coll_handle);

	if (status < 0)
		{
			ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS, davrods_resource_p->r,
					"rcOpenCollection failed: %d = %s", status,
					get_rods_error_msg(status));

			return dav_new_error (pool_p, HTTP_INTERNAL_SERVER_ERROR, 0, status, "Could not open a collection");
		}

	davrods_dir_conf_t *conf_p = davrods_resource_p->conf;
	struct HtmlTheme *theme_p = &(conf_p->theme);

	// Make brigade.
	apr_bucket_brigade *bb_p = apr_brigade_create (pool_p, output_p -> c -> bucket_alloc);
	apr_bucket *bkt;
	apr_status_t apr_ret;

	// Send start of HTML document.
	apr_brigade_printf(bb_p, NULL, NULL,
			"<!DOCTYPE html>\n<html lang=\"en\">\n<head><title>Index of %s on %s</title>\n",
			ap_escape_html(pool_p, davrods_resource_p->relative_uri),
			ap_escape_html(pool_p, conf_p->rods_zone));

	//    WHISPER("head \"%s\"", theme_p -> ht_head_s);
	//    WHISPER("top \"%s\"", theme_p -> ht_top_s);
	//    WHISPER("bottom \"%s\"", theme_p -> ht_bottom_s);
	//    WHISPER("coll \"%s\"", theme_p -> ht_collection_icon_s);
	//    WHISPER("obj \"%s\"", theme_p -> ht_object_icon_s);
	//    WHISPER("metadata \"%d\"", theme_p -> ht_show_metadata);

	/*
	 * If we have additional data for the <head> section, add it here.
	 */
	if (theme_p->ht_head_s)
		{
			apr_ret = apr_brigade_puts(bb_p, NULL, NULL, theme_p->ht_head_s);

			if (apr_ret != APR_SUCCESS)
				{
					ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS,
							davrods_resource_p->r,
							"Failed to add html to <head> section \"%s\"",
							theme_p->ht_head_s);

				} /* if (apr_ret != APR_SUCCESS) */

		} /* if (theme_p -> ht_head_s) */

	apr_brigade_puts(bb_p, NULL, NULL,
			"<body>\n\n"
					"<!-- Warning: Do not parse this directory listing programmatically,\n"
					"              the format may change without notice!\n"
					"              If you want to script access to these WebDAV collections,\n"
					"              please use the PROPFIND method instead. -->\n\n");

	/*
	 * If we have additional data to go above the directory listing, add it here.
	 */
	if (theme_p -> ht_top_s)
		{
			apr_ret = apr_brigade_puts (bb_p, NULL, NULL, theme_p -> ht_top_s);

			if (apr_ret != APR_SUCCESS)
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, davrods_resource_p -> r, "Failed to add html to top section \"%s\"", theme_p -> ht_top_s);
				} /* if (apr_ret != APR_SUCCESS) */

		}		/* if (theme_p -> ht_top_s) */

	apr_brigade_printf(bb_p, NULL, NULL,
			"<main>\n<h1>You are logged in as %s and browsing the index of %s on %s</h1>\n",
			davrods_resource_p->rods_conn->clientUser.userName,
			ap_escape_html (pool_p, davrods_resource_p->relative_uri),
			ap_escape_html (pool_p, conf_p->rods_zone));


	if (strcmp (davrods_resource_p->relative_uri, "/"))
		{
			apr_brigade_puts(bb_p, NULL, NULL, "<p><a href=\"..\">");

			if (theme_p -> ht_parent_icon_s)
				{
					apr_brigade_printf (bb_p, NULL, NULL, "<img src=\"%s\" alt=\"Browse to parent Collection\"/>", ap_escape_html (pool_p, theme_p -> ht_parent_icon_s));
				}
			else
				{
					apr_brigade_puts(bb_p, NULL, NULL, "↖");
				}

			apr_brigade_puts(bb_p, NULL, NULL, " Parent collection</a></p>\n");

		}		/* if (strcmp (davrods_resource_p->relative_uri, "/")) */

	/*
	 * Add the listing class
	 */
	apr_ret = apr_brigade_printf (bb_p, NULL, NULL, "<table class=\"%s\">\n<thead>\n<tr>", theme_p -> ht_listing_class_s ? theme_p -> ht_listing_class_s : "listing");
	if (apr_ret != APR_SUCCESS)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, davrods_resource_p -> r, "Failed to add start of table listing, %d", apr_ret);
		} /* if (apr_ret != APR_SUCCESS) */


	/*
	 * If we are going to display icons, add the column
	 */
	if ((theme_p -> ht_collection_icon_s) || (theme_p -> ht_object_icon_s))
		{
			apr_ret = apr_brigade_puts (bb_p, NULL, NULL, "<th class=\"icon\"></th>");

			if (apr_ret != APR_SUCCESS)
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, davrods_resource_p -> r, "Failed to add table header for icons, %d", apr_ret);
				} /* if (apr_ret != APR_SUCCESS) */

		}		/* if ((theme_p -> ht_collection_icon_s) || (theme_p -> ht_object_icon_s)) */

	apr_brigade_puts (bb_p, NULL, NULL, "<th class=\"name\">Name</th><th class=\"size\">Size</th><th class=\"owner\">Owner</th><th class=\"datestamp\">Last modified</th>");

	if (theme_p -> ht_show_metadata)
		{
			apr_ret = apr_brigade_puts (bb_p, NULL, NULL, "<th class=\"metadata\">Properties</th>");

			if (apr_ret != APR_SUCCESS)
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, davrods_resource_p -> r, "Failed to add table header for metadata, %d", apr_ret);
				} /* if (apr_ret != APR_SUCCESS) */

		}		/* if (theme_p->ht_show_metadata) */


	apr_brigade_puts(bb_p, NULL, NULL, "</tr>\n</thead>\n<tbody>\n");

	// Actually print the directory listing, one table row at a time.
	do
		{
			status = rclReadCollection(davrods_resource_p->rods_conn, &coll_handle,
					&coll_entry);

			if (status < 0)
				{
					if (status == CAT_NO_ROWS_FOUND)
						{
							// End of collection.
						}
					else
						{
							ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS,
									davrods_resource_p->r,
									"rcReadCollection failed for collection <%s> with error <%s>",
									davrods_resource_p->rods_path, get_rods_error_msg(status));

							apr_brigade_destroy(bb_p);

							return dav_new_error(pool_p, HTTP_INTERNAL_SERVER_ERROR,
									0, 0, "Could not read a collection entry from a collection.");
						}
				}
			else
				{
					apr_array_header_t *metadata_array_p = NULL;

					apr_brigade_puts(bb_p, NULL, NULL, "  <tr>");

					const char *name =
							coll_entry.objType == DATA_OBJ_T ?
									coll_entry.dataName : get_basename(coll_entry.collName);

					// Generate link.
					if (coll_entry.objType == COLL_OBJ_T)
						{
							// Collection links need a trailing slash for the '..' links to work correctly.

							if (theme_p->ht_collection_icon_s)
								{
									apr_brigade_printf(bb_p, NULL, NULL,
											"<td class=\"icon\"><img src=\"%s\" alt=\"iRods Collection\"></td>",
											ap_escape_html(pool_p, theme_p->ht_collection_icon_s));
								}

							apr_brigade_printf(bb_p, NULL, NULL,
									"<td class=\"name\"><a href=\"%s/\">%s/</a></td>",
									ap_escape_html(pool_p, ap_escape_uri(pool_p, name)),
									ap_escape_html(pool_p, name));

						}
					else
						{

							if (theme_p->ht_object_icon_s)
								{
									apr_brigade_printf(bb_p, NULL, NULL,
											"<td class=\"icon\"><img src=\"%s\" alt=\"iRods Data Object\"></td>",
											ap_escape_html(pool_p, theme_p->ht_object_icon_s));
								}

							apr_brigade_printf(bb_p, NULL, NULL,
									"<td class=\"name\"><a href=\"%s\">%s</a></td>",
									ap_escape_html(pool_p, ap_escape_uri(pool_p, name)),
									ap_escape_html(pool_p, name));
						}

					// Print data object size.
					if (coll_entry.objType == DATA_OBJ_T)
						{

							char size_buf [5] = { 0 };
							// Fancy file size formatting.
							apr_strfsize(coll_entry.dataSize, size_buf);
							if (size_buf [0])
								apr_brigade_printf(bb_p, NULL, NULL,
										"<td class=\"size\">%sB</td>", size_buf);
							else
								apr_brigade_printf(bb_p, NULL, NULL,
										"<td class=\"size\">%luB</td>", coll_entry.dataSize);
						}
					else
						{
							apr_brigade_puts(bb_p, NULL, NULL, "<td class=\"size\"></td>");
						}

					// Print owner.
					apr_brigade_printf(bb_p, NULL, NULL, "<td class=\"owner\">%s</td>",
							ap_escape_html(pool_p, coll_entry.ownerName));

					// Print modified-date string.
					uint64_t timestamp = atoll(coll_entry.modifyTime);
					apr_time_t apr_time = 0;
					apr_time_exp_t exploded = { 0 };
					char date_str [64] = { 0 };

					apr_time_ansi_put(&apr_time, timestamp);
					apr_time_exp_lt(&exploded, apr_time);

					size_t ret_size;
					if (!apr_strftime(date_str, &ret_size, sizeof(date_str),
							"%Y-%m-%d %H:%M", &exploded))
						{
							apr_brigade_printf(bb_p, NULL, NULL,
									"<td class=\"datestamp\">%s</td>",
									ap_escape_html(pool_p, date_str));
						}
					else
						{
							// Fallback, just in case.
							static_assert(sizeof(date_str) >= APR_RFC822_DATE_LEN,
									"Size of date_str buffer too low for RFC822 date");
							int status = apr_rfc822_date(date_str, timestamp * 1000 * 1000);
							apr_brigade_printf(bb_p, NULL, NULL,
									"<td class=\"datestamp\">%s</td>",
									ap_escape_html(pool_p,
											status >= 0 ?
													date_str : "Thu, 01 Jan 1970 00:00:00 GMT"));
						}

					if (theme_p->ht_show_metadata)
						{

							apr_brigade_puts(bb_p, NULL, NULL, "<td class=\"metadata\">");

							metadata_array_p = GetMetadata (resource_p, &coll_entry);

							if (metadata_array_p)
								{
									/*
									 * Sort the metadata keys into alphabetical order
									 */
									if (!apr_is_empty_array (metadata_array_p))
										{
											IrodsMetadata **metadata_pp = (IrodsMetadata **) apr_palloc (pool_p, (metadata_array_p -> nelts) * sizeof (IrodsMetadata **));

											if (metadata_pp)
												{
													int i;
													IrodsMetadata **md_pp = metadata_pp;

													for (i = 0; i < metadata_array_p -> nelts; ++ i, ++ md_pp)
														{
															*md_pp = ((IrodsMetadata **) metadata_array_p -> elts) [i];
														}

													qsort (metadata_pp, metadata_array_p -> nelts, sizeof (IrodsMetadata **), CompareIrodsMetadata);

													md_pp = metadata_pp;

													PrintMetadata (bb_p, metadata_pp, metadata_array_p -> nelts);

												}		/* if (metadata_pp) */

										}		/* if (!apr_is_empty_array (metadata_array_p)) */

								}		/* if (metadata_array_p) */

							apr_brigade_puts(bb_p, NULL, NULL, "</td>");
						}

					apr_brigade_puts(bb_p, NULL, NULL, "</tr>\n");
				}
		}
	while (status >= 0);

	// End HTML document.
	apr_brigade_puts(bb_p, NULL, NULL, "</tbody>\n</table>\n</main>\n");

	if (theme_p -> ht_bottom_s)
		{
			apr_ret = apr_brigade_puts(bb_p, NULL, NULL, theme_p->ht_bottom_s);

			if (apr_ret != APR_SUCCESS)
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, davrods_resource_p->r, "Failed to add html to bottom section \"%s\", %d", theme_p -> ht_bottom_s, apr_ret);
				} /* if (apr_ret != APR_SUCCESS) */

		}		/* if (theme_p -> ht_bottom_s) */

	apr_brigade_puts(bb_p, NULL, NULL, "\n</body>\n</html>\n");

	// Flush.
	if ((status = ap_pass_brigade(output_p, bb_p)) != APR_SUCCESS)
		{
			apr_brigade_destroy(bb_p);
			return dav_new_error(pool_p, HTTP_INTERNAL_SERVER_ERROR, 0, status,
					"Could not write contents to filter.");
		}

	bkt = apr_bucket_eos_create(output_p->c->bucket_alloc);

	APR_BRIGADE_INSERT_TAIL(bb_p, bkt);

	if ((status = ap_pass_brigade(output_p, bb_p)) != APR_SUCCESS)
		{
			apr_brigade_destroy (bb_p);
			return dav_new_error(pool_p, HTTP_INTERNAL_SERVER_ERROR, 0, status,
					"Could not write content to filter.");
		}
	apr_brigade_destroy(bb_p);

	return NULL;
}


static void PrintMetadata (apr_bucket_brigade *bb_p, IrodsMetadata **metadata_pp, int size)
{
	apr_brigade_puts (bb_p, NULL, NULL, "<ul class=\"metadata\">");

	for ( ; size > 0; -- size, ++ metadata_pp)
		{
			const IrodsMetadata *metadata_p = *metadata_pp;

			apr_brigade_printf (bb_p, NULL, NULL, "<li><span class=\"key\">%s</span>: <span class=\"value\">%s</span>", metadata_p -> im_key_s, metadata_p -> im_value_s);

			if (metadata_p -> im_units_s)
				{
					apr_brigade_printf (bb_p, NULL, NULL, "<span class=\"units\">%s</span>", metadata_p -> im_units_s);
				}

			apr_brigade_puts (bb_p, NULL, NULL, "</li>");
		}

	apr_brigade_puts (bb_p, NULL, NULL, "</ul>");
}


static int CompareIrodsMetadata (const void *v0_p, const void *v1_p)
{
	int res = 0;
	IrodsMetadata *md0_p = * ((IrodsMetadata **) v0_p);
	IrodsMetadata *md1_p = * ((IrodsMetadata **) v1_p);

	res = strcasecmp (md0_p -> im_key_s, md1_p -> im_key_s);

	if (res == 0)
		{
			res = strcasecmp (md0_p -> im_value_s, md1_p -> im_value_s);
		}

	return res;
}
