/*
 * theme.c
 *
 *  Created on: 28 Sep 2016
 *      Author: billy
 */

#include "theme.h"
#include "meta.h"
#include "repo.h"
#include "common.h"
#include "config.h"
#include "auth.h"
#include "rest.h"

#include "listing.h"

static const char *S_FILE_PREFIX_S = "file:";

/************************************/

static int AreIconsDisplayed (const struct HtmlTheme *theme_p);

static apr_status_t PrintParentLink (const char *icon_s, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p, apr_pool_t *pool_p);

static int PrintTableEntryToOption (void *data_p, const char *key_s, const char *value_s);

static apr_status_t PrintMetadataEditor (struct HtmlTheme *theme_p, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p);

static apr_status_t PrintSection (const char *value_s, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p);


/*************************************/


struct HtmlTheme *AllocateHtmlTheme (apr_pool_t *pool_p)
{
	struct HtmlTheme *theme_p = (struct HtmlTheme *) apr_pcalloc (pool_p, sizeof (struct HtmlTheme));

	if (theme_p)
		{
		  theme_p -> ht_head_s = NULL;
		  theme_p -> ht_top_s = NULL;
		  theme_p -> ht_bottom_s = NULL;
		  theme_p -> ht_collection_icon_s = NULL;
		  theme_p -> ht_object_icon_s = NULL;
		  theme_p -> ht_parent_icon_s = NULL;
		  theme_p -> ht_listing_class_s = NULL;
		  theme_p -> ht_add_metadata_icon_s = NULL;
		  theme_p -> ht_edit_metadata_icon_s = NULL;
		  theme_p -> ht_delete_metadata_icon_s = NULL;
		  theme_p -> ht_show_metadata_flag = MD_NONE;
		  theme_p -> ht_rest_api_s = NULL;

		  theme_p -> ht_ok_icon_s = NULL;
		  theme_p -> ht_cancel_icon_s = NULL;

		  theme_p -> ht_show_ids_flag = 0;
		  theme_p -> ht_add_search_form_flag = 1;
		  theme_p -> ht_icons_map_p = NULL;

		}

	return theme_p;
}


dav_error *DeliverThemedDirectory (const dav_resource *resource_p, ap_filter_t *output_p)
{
	struct dav_resource_private *davrods_resource_p = (struct dav_resource_private *) resource_p -> info;
	request_rec *req_p = davrods_resource_p -> r;
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
			ap_log_rerror (APLOG_MARK, APLOG_ERR, APR_SUCCESS, req_p, "rcOpenCollection failed: %d = %s", status, get_rods_error_msg(status));

			return dav_new_error (pool_p, HTTP_INTERNAL_SERVER_ERROR, 0, status, "Could not open a collection");
		}

	davrods_dir_conf_t *conf_p = davrods_resource_p->conf;

	const char * const user_s = davrods_resource_p -> rods_conn -> clientUser.userName;

	// Make brigade.
	apr_bucket_brigade *bucket_brigade_p = apr_brigade_create (pool_p, output_p -> c -> bucket_alloc);
	apr_status_t apr_status = PrintAllHTMLBeforeListing (davrods_resource_p, NULL, user_s, conf_p, req_p, bucket_brigade_p, pool_p);


	if (apr_status == APR_SUCCESS)
		{
			const char *davrods_root_path_s = davrods_resource_p -> root_dir;
			const char *exposed_root_s = GetRodsExposedPath (req_p);
			char *metadata_link_s = apr_pstrcat (pool_p, davrods_resource_p -> root_dir, conf_p -> davrods_api_path_s, REST_METADATA_SEARCH_S, NULL);
			IRodsConfig irods_config;

			if (SetIRodsConfig (&irods_config, exposed_root_s, davrods_root_path_s, metadata_link_s) == APR_SUCCESS)
				{
					int row_index = 0;

					// Actually print the directory listing, one table row at a time.
					do
						{
							status = rclReadCollection (davrods_resource_p -> rods_conn, &coll_handle, &coll_entry);

							if (status >= 0)
								{
									IRodsObject irods_obj;

									apr_status = SetIRodsObjectFromCollEntry (&irods_obj, &coll_entry, davrods_resource_p -> rods_conn, pool_p);

									if (apr_status == APR_SUCCESS)
										{
											apr_status = PrintItem (conf_p -> theme_p, &irods_obj, &irods_config, row_index, bucket_brigade_p, pool_p, resource_p -> info -> rods_conn, req_p);
											++ row_index;

											if (apr_status != APR_SUCCESS)
												{
													ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "Failed to PrintItem for \"%s\":\"%s\"",
																				 coll_entry.collName ? coll_entry.collName : "",
																				 coll_entry.dataName ? coll_entry.dataName : "");
												}
										}
									else
										{
											ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "Failed to SetIRodsObjectFromCollEntry for \"%s\":\"%s\"",
																		 coll_entry.collName ? coll_entry.collName : "",
																		 coll_entry.dataName ? coll_entry.dataName : "");
										}

								}
							else
								{
									if (status == CAT_NO_ROWS_FOUND)
										{
											// End of collection.
										}
									else
										{
											ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS,
													req_p,
													"rcReadCollection failed for collection <%s> with error <%s>",
													davrods_resource_p->rods_path, get_rods_error_msg(status));

											apr_brigade_destroy(bucket_brigade_p);

											return dav_new_error(pool_p, HTTP_INTERNAL_SERVER_ERROR,
													0, 0, "Could not read a collection entry from a collection.");
										}
								}
						}
					while (status >= 0);

				}		/* if (SetIRodsConfig (&irods_config, exposed_root_s, davrods_root_path_s, REST_METADATA_PATH_S)) */
			else
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "SetIRodsConfig failed for exposed_root_s:\"%s\" davrods_root_path_s:\"%s\"",
												 exposed_root_s ? exposed_root_s : "<NULL>",
												 davrods_root_path_s ? davrods_root_path_s: "<NULL>");
				}

		}		/* if (apr_status == APR_SUCCESS) */
	else
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "PrintAllHTMLBeforeListing failed");
		}

	apr_status = PrintAllHTMLAfterListing (conf_p -> theme_p, req_p, bucket_brigade_p, pool_p);
	if (apr_status != APR_SUCCESS)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "PrintAllHTMLAfterListing failed");
		}


	CloseBucketsStream (bucket_brigade_p);

	if ((status = ap_pass_brigade (output_p, bucket_brigade_p)) != APR_SUCCESS)
		{
			apr_brigade_destroy (bucket_brigade_p);
			return dav_new_error(pool_p, HTTP_INTERNAL_SERVER_ERROR, 0, status,
					"Could not write content to filter.");
		}
	apr_brigade_destroy(bucket_brigade_p);

	return NULL;
}


apr_status_t PrintAllHTMLAfterListing (struct HtmlTheme *theme_p, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p, apr_pool_t *pool_p)
{
	const char * const table_end_s = "</tbody>\n</table>\n</main>\n";

	apr_status_t apr_status = PrintBasicStringToBucketBrigade (table_end_s, bucket_brigade_p, req_p, __FILE__, __LINE__);

	if (apr_status == APR_SUCCESS)
		{
			apr_status = PrintMetadataEditor (theme_p, req_p, bucket_brigade_p);

			if (theme_p -> ht_bottom_s)
				{
					apr_status = PrintSection (theme_p -> ht_bottom_s, req_p, bucket_brigade_p);

					if (apr_status != APR_SUCCESS)
						{
							return apr_status;
						} /* if (apr_ret != APR_SUCCESS) */

				}		/* if (theme_p -> ht_bottom_s) */



			apr_status =  PrintBasicStringToBucketBrigade ("\n</body>\n</html>\n", bucket_brigade_p, req_p, __FILE__, __LINE__);
		}
	else
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "PrintBasicStringToBucketBrigade failed for \"%s\"", table_end_s);
		}

	return apr_status;
}


static apr_status_t PrintMetadataEditor (struct HtmlTheme *theme_p, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p)
{
	apr_status_t status;

	/*
	const char * const form_s = "<div id=\"edit_metadata_pop_up\">\n"
		"<form method=\"get\" id=\"metadata_editor\" action=\"/davrods/api/metadata/add\">\n"
		"<fieldset>\n"
		"<legend id=\"metadata_editor_title\">Edit Metadata</legend>\n"
		"<span class=\"add_group row\"><label for=\"attribute_editor\">Attribute:</label><input type=\"text\" name=\"key\" id=\"attribute_editor\" /></span>\n"
		"<span class=\"add_group row\"><label for=\"value_editor\">Value:</label><input type=\"text\" name=\"value\" id=\"value_editor\" /></span>\n"
		"<span class=\"add_group row\"><label for=\"units_editor\">Units:</label><input type=\"text\" name=\"units\"id=\"units_editor\" /></span>\n"
		"<span class=\"edit_group row\"><label for=\"new_attribute_editor\">Attribute:</label><input type=\"text\" name=\"new_key\" id=\"new_attribute_editor\" /></span>\n"
		"<span class=\"edit_group row\"><label for=\"new_value_editor\">Value:</label><input type=\"text\" name=\"new_value\" id=\"new_value_editor\" /></span>\n"
		"<span class=\"edit_group row\"><label for=\"new_units_editor\">Units:</label><input type=\"text\" name=\"new_units\" id=\"new_units_editor\" /></span>\n"
		"<input type=\"hidden\" name=\"id\" id=\"id_editor\" />\n"
		"</fieldset>\n"
		"<div class=\"toolbar\">";
	*/

	const char * const form_s = "<div id=\"edit_metadata_pop_up\">\n"
		"<form method=\"get\" id=\"metadata_editor\" action=\"/davrods/api/metadata/add\">\n"
		"<fieldset>\n"
		"<legend id=\"metadata_editor_title\">Edit Metadata</legend>\n"
		"<div class=\"add_group\">\n"
		"<div class=\"row\"><label for=\"attribute_editor\">Attribute:</label><input type=\"text\" name=\"key\" id=\"attribute_editor\" /></div>\n"
		"<div class=\"row\"><label for=\"value_editor\">Value:</label><input type=\"text\" name=\"value\" id=\"value_editor\" /></div>\n"
		"<div class=\"row\"><label for=\"units_editor\">Units:</label><input type=\"text\" name=\"units\" id=\"units_editor\" /></div>\n"
		"</div>\n"
		"<div class=\"edit_group\">\n"
		"<div class=\"row\"><label for=\"new_attribute_editor\">Attribute:</label><input type=\"text\" name=\"new_key\" id=\"new_attribute_editor\" /></div>\n"
		"<div class=\"row\"><label for=\"new_value_editor\">Value:</label><input type=\"text\" name=\"new_value\" id=\"new_value_editor\" /></div>\n"
		"<div class=\"row\"><label for=\"new_units_editor\">Units:</label><input type=\"text\" name=\"new_units\" id=\"new_units_editor\" /></div>\n"
		"</div>\n"
		"<input type=\"hidden\" name=\"id\" id=\"id_editor\" />\n"
		"</fieldset>\n"
		"<div class=\"toolbar\">";

	status = PrintBasicStringToBucketBrigade (form_s, bucket_brigade_p, req_p, __FILE__, __LINE__);

	if (theme_p -> ht_ok_icon_s)
		{
			status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<img class=\"button\" src=\"%s\" alt=\"Save Metadata\" title=\"Save the changes for this metadata key-value pair\" id=\"save_metadata\" /> Ok", theme_p -> ht_ok_icon_s);
		}
	else
		{
			PrintBasicStringToBucketBrigade ("<input type=\"button\" value=\"Save\" id=\"save_metadata\" />", bucket_brigade_p, req_p, __FILE__, __LINE__);
		}

	if (theme_p -> ht_cancel_icon_s)
		{
			status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<img class=\"button\" src=\"%s\" alt=\"Discard\" title=\"Discard any changes\" id=\"cancel_metadata\" /> Cancel", theme_p -> ht_cancel_icon_s);
		}
	else
		{
			PrintBasicStringToBucketBrigade ("<input type=\"button\" value=\"Cancel\" id=\"cancel_metadata\" />", bucket_brigade_p, req_p, __FILE__, __LINE__);
		}


	status = PrintBasicStringToBucketBrigade ("</div>\n</form>\n</div>\n", bucket_brigade_p, req_p, __FILE__, __LINE__);

	return status;
}


char *GetLocationPath (request_rec *req_p, davrods_dir_conf_t *conf_p, apr_pool_t *pool_p, const char *needle_s)
{
	char *davrods_path_s = NULL;
	char *metadata_path_s = apr_pstrcat (pool_p, conf_p -> davrods_api_path_s, needle_s, NULL);

	if (metadata_path_s)
		{
			char *api_in_uri_s = strstr (req_p -> uri, metadata_path_s);

			if (api_in_uri_s)
				{
					davrods_path_s = apr_pstrndup (pool_p, req_p -> uri, api_in_uri_s - (req_p -> uri));
				}
		}

	return davrods_path_s;
}


static apr_status_t PrintSection (const char *value_s, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p)
{
	apr_status_t status = APR_SUCCESS;

	if (value_s)
		{
			size_t l = strlen (S_FILE_PREFIX_S);

			if (strncmp (S_FILE_PREFIX_S, value_s, l) == 0)
				{
					status = PrintFileToBucketBrigade (value_s + l, bucket_brigade_p, req_p, __FILE__, __LINE__);
				}
			else
				{
					status = PrintBasicStringToBucketBrigade (value_s, bucket_brigade_p, req_p, __FILE__, __LINE__);
				}
		}

	return status;
}


apr_status_t PrintAllHTMLBeforeListing (struct dav_resource_private *davrods_resource_p, const char * const page_title_s, const char * const user_s, davrods_dir_conf_t *conf_p, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p, apr_pool_t *pool_p)
{
	// Send start of HTML document.
	const char *escaped_page_title_s = "";
	const char *escaped_zone_s = ap_escape_html (pool_p, conf_p -> rods_zone);
	const char * const api_path_s = conf_p -> davrods_api_path_s;


	if (davrods_resource_p)
		{
			escaped_page_title_s = ap_escape_html (pool_p, davrods_resource_p -> relative_uri);
		}
	else if (page_title_s)
		{
			escaped_page_title_s = ap_escape_html (pool_p, page_title_s);
		}
	/*
	 * Print the start of the doc
	 */
	apr_status_t apr_status =	apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<!DOCTYPE html>\n<html lang=\"en\">\n<head><title>Index of %s on %s</title>\n", escaped_page_title_s, escaped_zone_s);
	if (apr_status != APR_SUCCESS)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "Failed to add start of html doc with relative uri \"%s\" and zone \"%s\"", escaped_page_title_s, escaped_zone_s);

			return apr_status;
		}


	/*
	 * If we have additional data for the <head> section, add it here.
	 */
	if (conf_p -> theme_p -> ht_head_s)
		{
			apr_status = PrintSection (conf_p -> theme_p -> ht_head_s, req_p, bucket_brigade_p);

			if (apr_status != APR_SUCCESS)
				{
					return apr_status;
				} /* if (apr_ret != APR_SUCCESS) */

		} /* if (theme_p -> ht_head_s) */


	/*
	 * Write the start of the body section
	 */
	apr_status = PrintBasicStringToBucketBrigade ("<body>\n\n"
			"<!-- Warning: Do not parse this directory listing programmatically,\n"
			"              the format may change without notice!\n"
			"              If you want to script access to these WebDAV collections,\n"
			"              please use the PROPFIND method instead. -->\n\n",
			bucket_brigade_p, req_p, __FILE__, __LINE__);

	if (apr_status != APR_SUCCESS)
		{
			return apr_status;
		}


	/*
	 * If we have additional data to go above the directory listing, add it here.
	 */
	if (conf_p -> theme_p -> ht_top_s)
		{
			apr_status = PrintSection (conf_p -> theme_p -> ht_top_s, req_p, bucket_brigade_p);

			if (apr_status != APR_SUCCESS)
				{
					return apr_status;
				} /* if (apr_ret != APR_SUCCESS) */

		}		/* if (theme_p -> ht_top_s) */


	if (conf_p -> theme_p -> ht_add_search_form_flag)
		{
			apr_pool_t *davrods_pool_p = GetDavrodsMemoryPool (req_p);

			if (davrods_pool_p)
				{
					rcComm_t *connection_p  = GetIRODSConnectionFromPool (davrods_pool_p);

					if (connection_p)
						{
							apr_array_header_t *keys_p = GetAllDataObjectMetadataKeys (req_p -> pool, connection_p);

							if (keys_p)
								{
									/* Get the Location path where davrods is hosted */
									const char *davrods_path_s = NULL;
									const char *first_delimiter_s = "/";
									const char *second_delimiter_s = "/";
									int i = 0;

									if (davrods_resource_p)
										{
											davrods_path_s = davrods_resource_p -> root_dir;
										}		/* if (davrods_resource_p) */
									else
										{
											davrods_path_s = GetLocationPath (req_p, conf_p, pool_p, REST_METADATA_SEARCH_S);
										}

									/*
									 * make sure we don't have double forward-slash in the form action
									 *
									 * davrods_path_s/api_path_s
									 */
									if (davrods_path_s)
										{
											size_t davrods_path_length = strlen (davrods_path_s);

											if (* (davrods_path_s + (davrods_path_length - 1)) == '/')
												{
													if ((api_path_s) && (*api_path_s == '/'))
														{
															first_delimiter_s = "\b";
														}
													else
														{
															first_delimiter_s = "";
														}

													i = 1;
												}
										}

									if (api_path_s)
										{
											size_t api_path_length = strlen (api_path_s);

											if (i == 0)
												{
													if (*api_path_s == '/')
														{
															first_delimiter_s = "";
														}
												}

											if (* (api_path_s + (api_path_length - 1)) == '/')
												{
													second_delimiter_s = "";
												}
										}

									apr_status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<form action=\"%s%s%s%s%s\" class=\"search_form\">\nSearch: <select name=\"key\">\n", davrods_path_s, first_delimiter_s, api_path_s, second_delimiter_s, REST_METADATA_SEARCH_S);

							    for (i = 0; i < keys_p -> nelts; ++ i)
							    	{
							    		char *value_s = ((char **) keys_p -> elts) [i];
											apr_status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<option>%s</option>\n", value_s);

											if (apr_status != APR_SUCCESS)
												{
													break;
												}
							    	}

									apr_status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "</select>\n<input type=\"text\" name=\"value\" /></form>");

								}
						}
				}

		}		/* if (theme_p -> ht_add_search_form_flag) */

	/*
	 * Print the user status
	 */
	if (strcmp (user_s, conf_p -> davrods_public_username_s) != 0)
		{
			apr_status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<main>\n<h1>You are logged in as %s and browsing the index of %s on %s</h1>\n", user_s, escaped_page_title_s, escaped_zone_s);
		}
	else
		{
			apr_status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<main>\n<h1>You are browsing the index of %s on %s</h1>\n", escaped_page_title_s, escaped_zone_s);
		}

	if (apr_status != APR_SUCCESS)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "Failed to add the user status with user \"%s\", uri \"%s\", zone \"%s\"", user_s, escaped_page_title_s);
			return apr_status;
		} /* if (apr_ret != APR_SUCCESS) */


	if ((davrods_resource_p != NULL) && (strcmp (davrods_resource_p -> relative_uri, "/")))
		{
			apr_status = PrintParentLink (conf_p -> theme_p -> ht_parent_icon_s, req_p, bucket_brigade_p, pool_p);

			if (apr_status != APR_SUCCESS)
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "Failed to print parent link");
					return apr_status;
				}

		}		/* if (strcmp (davrods_resource_p->relative_uri, "/")) */


	/*
	 * Add the listing class
	 */
	apr_status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<table id=\"listings_table\" class=\"%s%s\">\n<thead>\n<tr>", conf_p -> theme_p -> ht_listing_class_s ? conf_p -> theme_p -> ht_listing_class_s : "listing", conf_p -> theme_p -> ht_show_metadata_flag == MD_ON_DEMAND ? " ajax" : "");
	if (apr_status != APR_SUCCESS)
		{
			ap_log_rerror (APLOG_MARK, APLOG_ERR, apr_status, req_p, "Failed to add start of table listing with class \"%s\"",conf_p -> theme_p -> ht_listing_class_s ? conf_p -> theme_p -> ht_listing_class_s : "listing");
			return apr_status;
		} /* if (apr_ret != APR_SUCCESS) */




	/*
	 * If we are going to display icons, add the column
	 */
	if (AreIconsDisplayed (conf_p -> theme_p))
		{
			apr_status = PrintBasicStringToBucketBrigade ("<th class=\"icon\"></th>", bucket_brigade_p, req_p, __FILE__, __LINE__);

			if (apr_status != APR_SUCCESS)
				{
					return apr_status;
				} /* if (apr_ret != APR_SUCCESS) */

		}		/* if (AreIconsDisplayed (theme_p)) */


	apr_status = PrintBasicStringToBucketBrigade ("<th class=\"name\">Name</th><th class=\"size\">Size</th><th class=\"owner\">Owner</th><th class=\"datestamp\">Last modified</th>", bucket_brigade_p, req_p, __FILE__, __LINE__);
	if (apr_status != APR_SUCCESS)
		{
			return apr_status;
		} /* if (apr_ret != APR_SUCCESS) */


	if (conf_p -> theme_p -> ht_show_metadata_flag)
		{
			apr_status = PrintBasicStringToBucketBrigade ("<th class=\"properties\">Properties</th>", bucket_brigade_p, req_p, __FILE__, __LINE__);

			if (apr_status != APR_SUCCESS)
				{
					return apr_status;
				} /* if (apr_ret != APR_SUCCESS) */

		}		/* if (theme_p -> ht_show_metadata_flag) */


	apr_status = PrintBasicStringToBucketBrigade ("</tr>\n</thead>\n<tbody>\n", bucket_brigade_p, req_p, __FILE__, __LINE__);

	return apr_status;
}


static int PrintTableEntryToOption (void *data_p, const char *key_s, const char *value_s)
{
	apr_bucket_brigade *bucket_brigade_p = (apr_bucket_brigade *) data_p;
	apr_status_t status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<option>%s</option>\n", key_s);

	/* TRUE:continue iteration. FALSE:stop iteration */
	return (status == APR_SUCCESS) ? TRUE : 0;
}


static apr_status_t PrintParentLink (const char *icon_s, request_rec *req_p, apr_bucket_brigade *bucket_brigade_p, apr_pool_t *pool_p)
{
	apr_status_t status = PrintBasicStringToBucketBrigade ("<p><a href=\"..\">", bucket_brigade_p, req_p, __FILE__, __LINE__);

	if (status != APR_SUCCESS)
		{
			return status;
		}

	if (icon_s)
		{
			const char *escaped_icon_s = ap_escape_html (pool_p, icon_s);

			status = apr_brigade_printf (bucket_brigade_p, NULL, NULL, "<img src=\"%s\" alt=\"Browse to parent Collection\"/>", escaped_icon_s);

			if (status != APR_SUCCESS)
				{
					ap_log_rerror (APLOG_MARK, APLOG_ERR, status, req_p, "Failed to print icon \"%s\"", escaped_icon_s);
					return status;
				}
		}
	else
		{
			/* Print a north-west arrow */
			status = PrintBasicStringToBucketBrigade ("&#8598;", bucket_brigade_p, req_p, __FILE__, __LINE__);

			if (status != APR_SUCCESS)
				{
					return status;
				}
		}

	status = PrintBasicStringToBucketBrigade (" Parent collection</a></p>\n", bucket_brigade_p, req_p, __FILE__, __LINE__);

	return status;
}


apr_status_t PrintItem (struct HtmlTheme *theme_p, const IRodsObject *irods_obj_p, const IRodsConfig *config_p, unsigned int row_index, apr_bucket_brigade *bb_p, apr_pool_t *pool_p, rcComm_t *connection_p, request_rec *req_p)
{
	apr_status_t status = APR_SUCCESS;
	const char *link_suffix_s = irods_obj_p -> io_obj_type == COLL_OBJ_T ? "/" : NULL;
	const char *name_s = GetIRodsObjectDisplayName (irods_obj_p);
	char *timestamp_s = GetIRodsObjectLastModifiedTime (irods_obj_p, pool_p);
	char *size_s = GetIRodsObjectSizeAsString (irods_obj_p, pool_p);

	const char * const row_classes_ss [] = { "odd", "even" };
	const char *row_class_s = row_classes_ss [(row_index % 2 == 0) ? 0 : 1];

	if (theme_p -> ht_show_ids_flag)
		{
			status = apr_brigade_printf (bb_p, NULL, NULL, "<tr class=\"id %s\" id=\"%d.%s\">", row_class_s, irods_obj_p -> io_obj_type, irods_obj_p -> io_id_s);
		}
	else
		{
			status = apr_brigade_printf (bb_p, NULL, NULL, "<tr class=\"%s\" id=\"%d.%s\">", row_class_s, irods_obj_p -> io_obj_type, irods_obj_p -> io_id_s);
		}

	if (status != APR_SUCCESS)
		{
			return status;
		}


	if (name_s)
		{
			const char *icon_s = GetIRodsObjectIcon (irods_obj_p, theme_p);
			const char *alt_s = GetIRodsObjectAltText (irods_obj_p);
			char *relative_link_s = GetIRodsObjectRelativeLink (irods_obj_p, config_p, pool_p);

			// Collection links need a trailing slash for the '..' links to work correctly.
			if (icon_s)
				{
					apr_brigade_printf (bb_p, NULL, NULL, "<td class=\"icon\"><img src=\"%s\"", ap_escape_html (pool_p, icon_s));

					if (alt_s)
						{
							apr_brigade_printf (bb_p, NULL, NULL, " alt=\"%s\"", alt_s);
						}

					status = PrintBasicStringToBucketBrigade (" /></td>", bb_p, req_p, __FILE__, __LINE__);
					if (status != APR_SUCCESS)
						{
							return status;
						}
				}

			apr_brigade_printf(bb_p, NULL, NULL,
					"<td class=\"name\"><a href=\"%s\">%s%s</a></td>",
					relative_link_s,
					ap_escape_html (pool_p, name_s),
					link_suffix_s ? link_suffix_s : "");

		}		/* if (name_s) */

	// Print data object size.
	status = PrintBasicStringToBucketBrigade ("<td class=\"size\">", bb_p, req_p, __FILE__, __LINE__);
	if (status != APR_SUCCESS)
		{
			return status;
		}


	if (size_s)
		{
			apr_brigade_printf (bb_p, NULL, NULL, "%sB", size_s);
		}
	else if (irods_obj_p -> io_obj_type == DATA_OBJ_T)
		{
			apr_brigade_printf(bb_p, NULL, NULL, "%luB", irods_obj_p -> io_size);
		}

	status = PrintBasicStringToBucketBrigade ("</td>", bb_p, req_p, __FILE__, __LINE__);
	if (status != APR_SUCCESS)
		{
			return status;
		}


	// Print owner
	apr_brigade_printf (bb_p, NULL, NULL, "<td class=\"owner\">%s</td>", ap_escape_html (pool_p, irods_obj_p -> io_owner_name_s));

	if (timestamp_s)
		{
			apr_brigade_printf (bb_p, NULL, NULL, "<td class=\"time\">%s</td>", timestamp_s);
		}
	else
		{
			status = PrintBasicStringToBucketBrigade ("<td class=\"time\"></td>", bb_p, req_p, __FILE__, __LINE__);
			if (status != APR_SUCCESS)
				{
					return status;
				}
		}

	switch (theme_p -> ht_show_metadata_flag)
		{
			case MD_FULL:
				{
					const char *zone_s = NULL;

					status = GetAndPrintMetadataForIRodsObject (irods_obj_p, config_p -> ic_metadata_root_link_s, zone_s, theme_p, bb_p, connection_p, pool_p);

					if (status == APR_SUCCESS)
						{

						}
				}
				break;

			case MD_ON_DEMAND:
				{
					const char *zone_s = NULL;

					status = GetAndPrintMetadataRestLinkForIRodsObject (irods_obj_p, config_p -> ic_metadata_root_link_s, zone_s, bb_p, connection_p, pool_p);

					if (status == APR_SUCCESS)
						{

						}

				}

				break;

			default:
				break;
		}




	status = PrintBasicStringToBucketBrigade ("</tr>\n", bb_p, req_p, __FILE__, __LINE__);
	if (status != APR_SUCCESS)
		{
			return status;
		}

	return status;
}


void MergeThemeConfigs (davrods_dir_conf_t *conf_p, davrods_dir_conf_t *parent_p, davrods_dir_conf_t *child_p, apr_pool_t *pool_p)
{
	DAVRODS_PROP_MERGE(theme_p -> ht_head_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_top_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_bottom_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_collection_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_object_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_parent_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_add_metadata_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_edit_metadata_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_delete_metadata_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_ok_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_cancel_icon_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_listing_class_s);
	DAVRODS_PROP_MERGE(theme_p -> ht_show_metadata_flag);
	DAVRODS_PROP_MERGE(theme_p -> ht_show_ids_flag);

	if (child_p -> theme_p -> ht_icons_map_p)
		{
			if (parent_p -> theme_p -> ht_icons_map_p)
				{
					conf_p -> theme_p -> ht_icons_map_p = apr_table_overlay (pool_p, parent_p -> theme_p -> ht_icons_map_p, child_p -> theme_p -> ht_icons_map_p);
				}
			else
				{
					conf_p -> theme_p -> ht_icons_map_p = child_p -> theme_p -> ht_icons_map_p;
				}
		}
	else
		{
			if (parent_p -> theme_p -> ht_icons_map_p)
				{
					conf_p -> theme_p -> ht_icons_map_p = parent_p -> theme_p -> ht_icons_map_p;
				}
			else
				{
					conf_p -> theme_p -> ht_icons_map_p = NULL;
				}
		}
}




static int AreIconsDisplayed (const struct HtmlTheme *theme_p)
{
	return ((theme_p -> ht_collection_icon_s) || (theme_p -> ht_object_icon_s) || (theme_p -> ht_icons_map_p)) ? 1 : 0;
}
