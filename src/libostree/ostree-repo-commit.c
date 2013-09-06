/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <glib-unix.h>
#include "otutil.h"
#include "libgsystem.h"

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file-enumerator.h"
#include "ostree-checksum-input-stream.h"
#include "ostree-mutable-tree.h"

static gboolean
commit_loose_object_trusted (OstreeRepo        *self,
                             const char        *loose_path,
                             const char        *tempfile_name,
                             GCancellable      *cancellable,
                             GError           **error)
{
  gboolean ret = FALSE;
  char loose_prefix[3];

  loose_prefix[0] = loose_path[0];
  loose_prefix[1] = loose_path[1];
  loose_prefix[2] = '\0';
  if (G_UNLIKELY (mkdirat (self->objects_dir_fd, loose_prefix, 0777) == -1))
    {
      int errsv = errno;
      if (errsv != EEXIST)
        {
          ot_util_set_error_from_errno (error, errsv);
          goto out;
        }
    }

  if (G_UNLIKELY (renameat (self->tmp_dir_fd, tempfile_name,
                            self->objects_dir_fd, loose_path) == -1))
    {
      if (errno != EEXIST)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ", tempfile_name);
          goto out;
        }
      else
        (void) unlinkat (self->tmp_dir_fd, tempfile_name, 0);
    }

  ret = TRUE;
 out:
  return ret;
}

/* Create a randomly-named symbolic link in @tempdir which points to
 * @target.  The filename will be returned in @out_file.
 *
 * The reason this odd function exists is that the repo should only
 * contain objects in their final state.  For bare repositories, we
 * need to first create the symlink, then chown it, and apply all
 * extended attributes, before finally rename()ing it into place.
 */
static gboolean
make_temporary_symlink_at (int             tmp_dirfd,
                           const char     *target,
                           char          **out_name,
                           GCancellable   *cancellable,
                           GError        **error)
{
  gboolean ret = FALSE;
  gs_free char *tmpname = NULL;
  guint i;
  const int max_attempts = 128;

  for (i = 0; i < max_attempts; i++)
    {
      g_free (tmpname);
      tmpname = gsystem_fileutil_gen_tmp_name (NULL, NULL);
      if (symlinkat (target, tmp_dirfd, tmpname) < 0)
        {
          if (errno == EEXIST)
            continue;
          else
            {
              int errsv = errno;
              g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                                   g_strerror (errsv));
              goto out;
            }
        }
      else
        break;
    }
  if (i == max_attempts)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted attempts to open temporary file");
      goto out;
    }

  ret = TRUE;
  gs_transfer_out_value (out_name, &tmpname);
 out:
  return ret;
}

static gboolean
write_object (OstreeRepo         *self,
              OstreeObjectType    objtype,
              const char         *expected_checksum,
              GInputStream       *input,
              guint64             file_object_length,
              guchar            **out_csum,
              GCancellable       *cancellable,
              GError            **error)
{
  gboolean ret = FALSE;
  const char *actual_checksum;
  gboolean do_commit;
  OstreeRepoMode repo_mode;
  gs_free char *temp_filename = NULL;
  gs_unref_object GFile *temp_file = NULL;
  gs_unref_object GFile *stored_path = NULL;
  gs_free guchar *ret_csum = NULL;
  gs_unref_object OstreeChecksumInputStream *checksum_input = NULL;
  gs_unref_object GInputStream *file_input = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gboolean have_obj;
  GChecksum *checksum = NULL;
  gboolean temp_file_is_regular;
  gboolean is_symlink = FALSE;
  char loose_objpath[_OSTREE_LOOSE_PATH_MAX];

  g_return_val_if_fail (self->in_transaction, FALSE);
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_assert (expected_checksum || out_csum);

  if (expected_checksum)
    {
      if (!_ostree_repo_has_loose_object (self, expected_checksum, objtype,
                                          &have_obj, loose_objpath,
                                          cancellable, error))
        goto out;
      if (have_obj)
        {
          ret = TRUE;
          goto out;
        }
    }

  repo_mode = ostree_repo_get_mode (self);

  if (out_csum)
    {
      checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (input)
        checksum_input = ostree_checksum_input_stream_new (input, checksum);
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      if (!ostree_content_stream_parse (FALSE, checksum_input ? (GInputStream*)checksum_input : input,
                                        file_object_length, FALSE,
                                        &file_input, &file_info, &xattrs,
                                        cancellable, error))
        goto out;

      temp_file_is_regular = g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR;
      is_symlink = g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK;

      if (!(temp_file_is_regular || is_symlink))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Unsupported file type %u", g_file_info_get_file_type (file_info));
          goto out;
        }

      /* For regular files, we create them with default mode, and only
       * later apply any xattrs and setuid bits.  The rationale here
       * is that an attacker on the network with the ability to MITM
       * could potentially cause the system to make a temporary setuid
       * binary with trailing garbage, creating a window on the local
       * system where a malicious setuid binary exists.
       */
      if (repo_mode == OSTREE_REPO_MODE_BARE && temp_file_is_regular)
        {
          gs_unref_object GOutputStream *temp_out = NULL;
          if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, 0644, &temp_filename, &temp_out,
                                          cancellable, error))
            goto out;
          temp_file = g_file_get_child (self->tmp_dir, temp_filename);
          if (g_output_stream_splice (temp_out, file_input, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                      cancellable, error) < 0)
            goto out;
        }
      else if (repo_mode == OSTREE_REPO_MODE_BARE && is_symlink)
        {
          if (!make_temporary_symlink_at (self->tmp_dir_fd,
                                          g_file_info_get_symlink_target (file_info),
                                          &temp_filename,
                                          cancellable, error))
            goto out;
          temp_file = g_file_get_child (self->tmp_dir, temp_filename);
        }
      else if (repo_mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
        {
          gs_unref_variant GVariant *file_meta = NULL;
          gs_unref_object GOutputStream *temp_out = NULL;
          gs_unref_object GConverter *zlib_compressor = NULL;
          gs_unref_object GOutputStream *compressed_out_stream = NULL;

          if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, 0644,
                                          &temp_filename, &temp_out,
                                          cancellable, error))
            goto out;
          temp_file = g_file_get_child (self->tmp_dir, temp_filename);
          temp_file_is_regular = TRUE;

          file_meta = _ostree_zlib_file_header_new (file_info, xattrs);

          if (!_ostree_write_variant_with_size (temp_out, file_meta, 0, NULL, NULL,
                                                cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              zlib_compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 9);
              compressed_out_stream = g_converter_output_stream_new (temp_out, zlib_compressor);
              
              if (g_output_stream_splice (compressed_out_stream, file_input,
                                          G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                          cancellable, error) < 0)
                goto out;
            }

          if (!g_output_stream_close (temp_out, cancellable, error))
            goto out;
        }
      else
        g_assert_not_reached ();
    }
  else
    {
      gs_unref_object GOutputStream *temp_out = NULL;
      if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, 0644, &temp_filename, &temp_out,
                                      cancellable, error))
        goto out;
      temp_file = g_file_get_child (self->tmp_dir, temp_filename);
      if (g_output_stream_splice (temp_out, checksum_input ? (GInputStream*)checksum_input : input,
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                  cancellable, error) < 0)
        goto out;
      temp_file_is_regular = TRUE;
    }

  if (!checksum)
    actual_checksum = expected_checksum;
  else
    {
      actual_checksum = g_checksum_get_string (checksum);
      if (expected_checksum && strcmp (actual_checksum, expected_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted %s object %s (actual checksum is %s)",
                       ostree_object_type_to_string (objtype),
                       expected_checksum, actual_checksum);
          goto out;
        }
    }
          
  if (!_ostree_repo_has_loose_object (self, actual_checksum, objtype,
                                      &have_obj, loose_objpath,
                                      cancellable, error))
    goto out;
          
  do_commit = !have_obj;

  if (do_commit)
    {
      if (objtype == OSTREE_OBJECT_TYPE_FILE && repo_mode == OSTREE_REPO_MODE_BARE)
        {
          g_assert (file_info != NULL);

          /* Now that we know the checksum is valid, apply uid/gid, mode bits,
           * and extended attributes.
           */
          if (G_UNLIKELY (fchownat (self->tmp_dir_fd, temp_filename,
                                    g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                                    g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                                    AT_SYMLINK_NOFOLLOW) == -1))
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }

          /* Sadly we can't use at-relative API for xattrs because
           * there's no lsetxattrat.
           */
          if (xattrs != NULL)
            {
              if (!ostree_set_xattrs (temp_file, xattrs, cancellable, error))
                goto out;
            }

          /* symlinks are always 777, there's no lchmod().  Calling
           * chmod() on them would apply to their target, which we
           * definitely don't want.
           */
          if (!is_symlink)
            {
              int fd;
              int res;

              if (!gs_file_openat_noatime (self->tmp_dir_fd, temp_filename, &fd,
                                           cancellable, error))
                goto out;

              do
                res = fchmod (fd, g_file_info_get_attribute_uint32 (file_info, "unix::mode"));
              while (G_UNLIKELY (res == -1 && errno == EINTR));
              if (G_UNLIKELY (res == -1))
                {
                  (void) close (fd);
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }

              /* Ensure that in case of a power cut, these files have the data we
               * want.   See http://lwn.net/Articles/322823/
               */
              if (fsync (fd) == -1)
                {
                  (void) close (fd);
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }
              (void) close (fd);
            }
        }
      if (!commit_loose_object_trusted (self, loose_objpath, temp_filename,
                                        cancellable, error))
        goto out;
      g_clear_pointer (&temp_filename, g_free);
      g_clear_object (&temp_file);
    }

  g_mutex_lock (&self->txn_stats_lock);
  if (do_commit)
    {
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          self->txn_stats.metadata_objects_written++;
        }
      else
        {
          self->txn_stats.content_objects_written++;
          self->txn_stats.content_bytes_written += file_object_length;
        }
    }
  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    self->txn_stats.metadata_objects_total++;
  else
    self->txn_stats.content_objects_total++;
  g_mutex_unlock (&self->txn_stats_lock);
      
  if (checksum)
    ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  if (temp_filename)
    (void) unlinkat (self->tmp_dir_fd, temp_filename, 0);
  g_clear_pointer (&checksum, (GDestroyNotify) g_checksum_free);
  return ret;
}

typedef struct {
  dev_t dev;
  ino_t ino;
} OstreeDevIno;

static guint
devino_hash (gconstpointer a)
{
  OstreeDevIno *a_i = (gpointer)a;
  return (guint) (a_i->dev + a_i->ino);
}

static int
devino_equal (gconstpointer   a,
              gconstpointer   b)
{
  OstreeDevIno *a_i = (gpointer)a;
  OstreeDevIno *b_i = (gpointer)b;
  return a_i->dev == b_i->dev
    && a_i->ino == b_i->ino;
}

static gboolean
scan_loose_devino (OstreeRepo                     *self,
                   GHashTable                     *devino_cache,
                   GCancellable                   *cancellable,
                   GError                        **error)
{
  gboolean ret = FALSE;
  guint i;
  OstreeRepoMode repo_mode;
  gs_unref_ptrarray GPtrArray *object_dirs = NULL;

  if (self->parent_repo)
    {
      if (!scan_loose_devino (self->parent_repo, devino_cache, cancellable, error))
        goto out;
    }

  repo_mode = ostree_repo_get_mode (self);

  if (!_ostree_repo_get_loose_object_dirs (self, &object_dirs, cancellable, error))
    goto out;

  for (i = 0; i < object_dirs->len; i++)
    {
      GFile *objdir = object_dirs->pdata[i];
      gs_unref_object GFileEnumerator *enumerator = NULL;
      gs_unref_object GFileInfo *file_info = NULL;
      const char *dirname;

      enumerator = g_file_enumerate_children (objdir, OSTREE_GIO_FAST_QUERYINFO,
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable,
                                              error);
      if (!enumerator)
        goto out;

      dirname = gs_file_get_basename_cached (objdir);

      while (TRUE)
        {
          const char *name;
          const char *dot;
          guint32 type;
          OstreeDevIno *key;
          GString *checksum;
          gboolean skip;

          if (!gs_file_enumerator_iterate (enumerator, &file_info, NULL,
                                           NULL, error))
            goto out;
          if (file_info == NULL)
            break;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name");
          type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

          if (type == G_FILE_TYPE_DIRECTORY)
            continue;

          switch (repo_mode)
            {
            case OSTREE_REPO_MODE_ARCHIVE_Z2:
            case OSTREE_REPO_MODE_BARE:
              skip = !g_str_has_suffix (name, ".file");
              break;
            default:
              g_assert_not_reached ();
            }
          if (skip)
            continue;

          dot = strrchr (name, '.');
          g_assert (dot);

          if ((dot - name) != 62)
            continue;

          checksum = g_string_new (dirname);
          g_string_append_len (checksum, name, 62);

          key = g_new (OstreeDevIno, 1);
          key->dev = g_file_info_get_attribute_uint32 (file_info, "unix::device");
          key->ino = g_file_info_get_attribute_uint64 (file_info, "unix::inode");

          g_hash_table_replace (devino_cache, key, g_string_free (checksum, FALSE));
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static const char *
devino_cache_lookup (OstreeRepo           *self,
                     GFileInfo            *finfo)
{
  OstreeDevIno dev_ino;

  if (!self->loose_object_devino_hash)
    return NULL;

  dev_ino.dev = g_file_info_get_attribute_uint32 (finfo, "unix::device");
  dev_ino.ino = g_file_info_get_attribute_uint64 (finfo, "unix::inode");
  return g_hash_table_lookup (self->loose_object_devino_hash, &dev_ino);
}

/**
 * ostree_repo_scan_hardlinks:
 * @self: An #OstreeRepo
 * @cancellable: Cancellable
 * @error: Error
 *
 * When ostree builds a mutable tree from directory like in
 * ostree_repo_write_directory_to_mtree(), it has to scan all files that you
 * pass in and compute their checksums. If your commit contains hardlinks from
 * ostree's existing repo, ostree can build a mapping of device numbers and
 * inodes to their checksum.
 *
 * There is an upfront cost to creating this mapping, as this will scan the
 * entire objects directory. If your commit is composed of mostly hardlinks to
 * existing ostree objects, then this will speed up considerably, so call it
 * before you call ostree_write_directory_to_mtree() or similar.
 */
gboolean
ostree_repo_scan_hardlinks (OstreeRepo    *self,
                            GCancellable  *cancellable,
                            GError       **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  if (!self->loose_object_devino_hash)
    self->loose_object_devino_hash = g_hash_table_new_full (devino_hash, devino_equal, g_free, g_free);
  g_hash_table_remove_all (self->loose_object_devino_hash);
  if (!scan_loose_devino (self, self->loose_object_devino_hash, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_prepare_transaction:
 * @self: An #OstreeRepo
 * @out_transaction_resume: (allow-none) (out): Whether this transaction
 * is resuming from a previous one.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Starts or resumes a transaction. In order to write to a repo, you
 * need to start a transaction. You can complete the transaction with
 * ostree_repo_commit_transaction(), or abort the transaction with
 * ostree_repo_abort_transaction().
 *
 * Currently, transactions are not atomic, and aborting a transaction
 * will not erase any data you  write during the transaction.
 */
gboolean
ostree_repo_prepare_transaction (OstreeRepo     *self,
                                 gboolean       *out_transaction_resume,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  gboolean ret = FALSE;
  gboolean ret_transaction_resume = FALSE;
  gs_free char *transaction_str = NULL;

  g_return_val_if_fail (self->in_transaction == FALSE, FALSE);

  if (self->transaction_lock_path == NULL)
    self->transaction_lock_path = g_file_resolve_relative_path (self->repodir, "transaction");

  if (g_file_query_file_type (self->transaction_lock_path, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK)
    ret_transaction_resume = TRUE;
  else
    ret_transaction_resume = FALSE;

  memset (&self->txn_stats, 0, sizeof (OstreeRepoTransactionStats));

  self->in_transaction = TRUE;
  if (ret_transaction_resume)
    {
      if (!ot_gfile_ensure_unlinked (self->transaction_lock_path, cancellable, error))
        goto out;
    }
  transaction_str = g_strdup_printf ("pid=%llu", (unsigned long long) getpid ());
  if (!g_file_make_symbolic_link (self->transaction_lock_path, transaction_str,
                                  cancellable, error))
    goto out;

  ret = TRUE;
  if (out_transaction_resume)
    *out_transaction_resume = ret_transaction_resume;
 out:
  return ret;
}

static gboolean
cleanup_tmpdir (OstreeRepo        *self,
                GCancellable      *cancellable,
                GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *enumerator = NULL;

  enumerator = g_file_enumerate_children (self->tmp_dir, "standard::name",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable,
                                          error);
  if (!enumerator)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, &path,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      if (!gs_shutil_rm_rf (path, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
ensure_txn_refs (OstreeRepo *self)
{
  if (self->txn_refs == NULL)
    self->txn_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

/**
 * ostree_repo_transaction_set_refspec:
 * @self: An #OstreeRepo
 * @refspec: The refspec to write
 * @checksum: The checksum to point it to
 *
 * Like ostree_repo_transaction_set__ref(), but takes concatenated
 * @refspec format as input instead of separate remote and name
 * arguments.
 */
void
ostree_repo_transaction_set_refspec (OstreeRepo *self,
                                     const char *refspec,
                                     const char *checksum)
{
  g_return_if_fail (self->in_transaction == TRUE);

  ensure_txn_refs (self);

  g_hash_table_replace (self->txn_refs, g_strdup (refspec), g_strdup (checksum));
}

/**
 * ostree_repo_transaction_set_ref:
 * @self: An #OstreeRepo
 * @remote: (allow-none): A remote for the ref
 * @ref: The ref to write
 * @checksum: The checksum to point it to
 *
 * If @checksum is not %NULL, then record it as the target of ref named
 * @ref; if @remote is provided, the ref will appear to originate from that
 * remote.
 *
 * Otherwise, if @checksum is %NULL, then record that the ref should
 * be deleted.
 *
 * The change will not be written out immediately, but when the transaction
 * is completed with ostree_repo_complete_transaction(). If the transaction
 * is instead aborted with ostree_repo_abort_transaction(), no changes will
 * be made to the repository.
 */
void
ostree_repo_transaction_set_ref (OstreeRepo *self,
                                 const char *remote,
                                 const char *ref,
                                 const char *checksum)
{
  char *refspec;

  g_return_if_fail (self->in_transaction == TRUE);

  ensure_txn_refs (self);

  if (remote)
    refspec = g_strdup_printf ("%s:%s", remote, ref);
  else
    refspec = g_strdup (ref);

  g_hash_table_replace (self->txn_refs, refspec, g_strdup (checksum));
}

/**
 * ostree_repo_commit_transaction:
 * @self: An #OstreeRepo
 * @out_stats: (allow-none) (out): A set of statisitics of things
 * that happened during this transaction.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Complete the transaction. Any refs set with
 * ostree_repo_transaction_set_ref() or
 * ostree_repo_transaction_set_refspec() will be written out.
 */
gboolean
ostree_repo_commit_transaction (OstreeRepo                  *self,
                                OstreeRepoTransactionStats  *out_stats,
                                GCancellable                *cancellable,
                                GError                     **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  if (!cleanup_tmpdir (self, cancellable, error))
    goto out;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  if (self->txn_refs)
    if (!_ostree_repo_update_refs (self, self->txn_refs, cancellable, error))
      goto out;
  g_clear_pointer (&self->txn_refs, g_hash_table_destroy);

  self->in_transaction = FALSE;

  if (!ot_gfile_ensure_unlinked (self->transaction_lock_path, cancellable, error))
    goto out;

  if (out_stats)
    *out_stats = self->txn_stats;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_abort_transaction (OstreeRepo     *self,
                               GCancellable   *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;

  if (!self->in_transaction)
    return TRUE;

  if (!cleanup_tmpdir (self, cancellable, error))
    goto out;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  g_clear_pointer (&self->txn_refs, g_hash_table_destroy);

  self->in_transaction = FALSE;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_write_metadata:
 * @self: Repo
 * @objtype: Object type
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object: Metadata
 * @out_csum: (out) (array fixed-size=32) (allow-none): Binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @variant.  Return the checksum
 * as @out_csum.
 *
 * If @expected_checksum is not %NULL, verify it against the
 * computed checksum.
 */
gboolean
ostree_repo_write_metadata (OstreeRepo         *self,
                            OstreeObjectType    objtype,
                            const char         *expected_checksum,
                            GVariant           *object,
                            guchar            **out_csum,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *normalized = NULL;

  normalized = g_variant_get_normal_form (object);
  input = ot_variant_read (normalized);

  return write_object (self, objtype, expected_checksum, input, 0, out_csum,
                       cancellable, error);
}

/**
 * ostree_repo_write_metadata_trusted:
 * @self: Repo
 * @objtype: Object type
 * @checksum: Store object with this ASCII SHA256 checksum
 * @variant: Metadata object
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @variant; the provided @checksum is
 * trusted.
 */
gboolean
ostree_repo_write_metadata_trusted (OstreeRepo         *self,
                                    OstreeObjectType    type,
                                    const char         *checksum,
                                    GVariant           *variant,
                                    GCancellable       *cancellable,
                                    GError            **error)
{
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *normalized = NULL;

  normalized = g_variant_get_normal_form (variant);
  input = ot_variant_read (normalized);

  return write_object (self, type, checksum, input, 0, NULL,
                       cancellable, error);
}

typedef struct {
  OstreeRepo *repo;
  OstreeObjectType objtype;
  char *expected_checksum;
  GVariant *object;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;

  guchar *result_csum;
} WriteMetadataAsyncData;

static void
write_metadata_async_data_free (gpointer user_data)
{
  WriteMetadataAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_variant_unref (data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
write_metadata_thread (GSimpleAsyncResult  *res,
                       GObject             *object,
                       GCancellable        *cancellable)
{
  GError *error = NULL;
  WriteMetadataAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_write_metadata (data->repo, data->objtype, data->expected_checksum,
                                   data->object,
                                   &data->result_csum,
                                   cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_write_metadata_async:
 * @self: Repo
 * @objtype: Object type
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object: Metadata
 * @cancellable: Cancellable
 * @callback: Invoked when metadata is writed
 * @user_data: Data for @callback
 *
 * Asynchronously store the metadata object @variant.  If provided,
 * the checksum @expected_checksum will be verified.
 */
void
ostree_repo_write_metadata_async (OstreeRepo               *self,
                                  OstreeObjectType          objtype,
                                  const char               *expected_checksum,
                                  GVariant                 *object,
                                  GCancellable             *cancellable,
                                  GAsyncReadyCallback       callback,
                                  gpointer                  user_data)
{
  WriteMetadataAsyncData *asyncdata;

  asyncdata = g_new0 (WriteMetadataAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->objtype = objtype;
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_variant_ref (object);
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_write_metadata_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             write_metadata_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, write_metadata_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

gboolean
ostree_repo_write_metadata_finish (OstreeRepo        *self,
                                   GAsyncResult      *result,
                                   guchar           **out_csum,
                                   GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  WriteMetadataAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_write_metadata_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->result_csum;
  data->result_csum = NULL;
  return TRUE;
}

gboolean
_ostree_repo_write_directory_meta (OstreeRepo   *self,
                                   GFileInfo    *file_info,
                                   GVariant     *xattrs,
                                   guchar      **out_csum,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  gs_unref_variant GVariant *dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);

  return ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                     dirmeta, out_csum, cancellable, error);
}

GFile *
_ostree_repo_get_object_path (OstreeRepo       *self,
                              const char       *checksum,
                              OstreeObjectType  type)
{
  char *relpath;
  GFile *ret;
  gboolean compressed;

  compressed = (type == OSTREE_OBJECT_TYPE_FILE
                && ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE_Z2);
  relpath = ostree_get_relative_object_path (checksum, type, compressed);
  ret = g_file_resolve_relative_path (self->repodir, relpath);
  g_free (relpath);

  return ret;
}

GFile *
_ostree_repo_get_uncompressed_object_cache_path (OstreeRepo       *self,
                                                 const char       *checksum)
{
  char *relpath;
  GFile *ret;

  relpath = ostree_get_relative_object_path (checksum, OSTREE_OBJECT_TYPE_FILE, FALSE);
  ret = g_file_resolve_relative_path (self->uncompressed_objects_dir, relpath);
  g_free (relpath);

  return ret;
}

/**
 * ostree_repo_write_content_trusted:
 * @self: Repo
 * @checksum: Store content using this ASCII SHA256 checksum
 * @object_input: Content stream
 * @length: Length of @object_input
 * @cancellable: Cancellable
 * @error: Data for @callback
 *
 * Store the content object streamed as @object_input, with total
 * length @length.  The given @checksum will be treated as trusted.
 *
 * This function should be used when importing file objects from local
 * disk, for example.
 */
gboolean
ostree_repo_write_content_trusted (OstreeRepo       *self,
                                   const char       *checksum,
                                   GInputStream     *object_input,
                                   guint64           length,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  return write_object (self, OSTREE_OBJECT_TYPE_FILE, checksum,
                       object_input, length, NULL,
                       cancellable, error);
}

/**
 * ostree_repo_write_content:
 * @self: Repo
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object_input: Content object stream
 * @length: Length of @object_input
 * @out_csum: (out) (array fixed-size=32) (allow-none): Binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the content object streamed as @object_input,
 * with total length @length.  The actual checksum will
 * be returned as @out_csum.
 */
gboolean
ostree_repo_write_content (OstreeRepo       *self,
                           const char       *expected_checksum,
                           GInputStream     *object_input,
                           guint64           length,
                           guchar          **out_csum,
                           GCancellable     *cancellable,
                           GError          **error)
{
  return write_object (self, OSTREE_OBJECT_TYPE_FILE, expected_checksum,
                       object_input, length, out_csum,
                       cancellable, error);
}

typedef struct {
  OstreeRepo *repo;
  char *expected_checksum;
  GInputStream *object;
  guint64 file_object_length;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;

  guchar *result_csum;
} WriteContentAsyncData;

static void
write_content_async_data_free (gpointer user_data)
{
  WriteContentAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
write_content_thread (GSimpleAsyncResult  *res,
                      GObject             *object,
                      GCancellable        *cancellable)
{
  GError *error = NULL;
  WriteContentAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_write_content (data->repo, data->expected_checksum,
                                  data->object, data->file_object_length,
                                  &data->result_csum,
                                  cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_write_content_async:
 * @self: Repo
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object: Input
 * @length: Length of @object
 * @cancellable: Cancellable
 * @callback: Invoked when content is writed
 * @user_data: User data for @callback
 *
 * Asynchronously store the content object @object.  If provided, the
 * checksum @expected_checksum will be verified.
 */
void
ostree_repo_write_content_async (OstreeRepo               *self,
                                 const char               *expected_checksum,
                                 GInputStream             *object,
                                 guint64                   length,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data)
{
  WriteContentAsyncData *asyncdata;

  asyncdata = g_new0 (WriteContentAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_object_ref (object);
  asyncdata->file_object_length = length;
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_write_content_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             write_content_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, write_content_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

/**
 * ostree_repo_write_content_finish:
 * @self: a #OstreeRepo
 * @result: a #GAsyncResult
 * @out_csum: (out) (transfer full): A binary SHA256 checksum of the content object
 * @error: a #GError
 *
 * Completes an invocation of ostree_repo_write_content_async().
 */
gboolean
ostree_repo_write_content_finish (OstreeRepo        *self,
                                  GAsyncResult      *result,
                                  guchar           **out_csum,
                                  GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  WriteContentAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_write_content_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  ot_transfer_out_value (out_csum, &data->result_csum);
  return TRUE;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

/**
 * ostree_repo_write_commit:
 * @self: Repo
 * @branch: Name of ref
 * @parent: (allow-none): ASCII SHA256 checksum for parent, or %NULL for none
 * @subject: Subject
 * @body: Body
 * @root_contents_checksum: ASCII SHA256 checksum for %OSTREE_OBJECT_TYPE_DIR_TREE
 * @root_metadata_checksum: ASCII SHA256 checksum for %OSTREE_OBJECT_TYPE_DIR_META
 * @out_commit: (out): Resulting ASCII SHA256 checksum for commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write a commit metadata object, referencing @root_contents_checksum
 * and @root_metadata_checksum.
 */
gboolean
ostree_repo_write_commit (OstreeRepo *self,
                          const char   *branch,
                          const char   *parent,
                          const char   *subject,
                          const char   *body,
                          const char   *root_contents_checksum,
                          const char   *root_metadata_checksum,
                          char        **out_commit,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_commit = NULL;
  gs_unref_variant GVariant *commit = NULL;
  gs_free guchar *commit_csum = NULL;
  GDateTime *now = NULL;

  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (root_contents_checksum != NULL, FALSE);
  g_return_val_if_fail (root_metadata_checksum != NULL, FALSE);

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                          create_empty_gvariant_dict (),
                          parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
                          g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0),
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          ostree_checksum_to_bytes_v (root_contents_checksum),
                          ostree_checksum_to_bytes_v (root_metadata_checksum));
  g_variant_ref_sink (commit);
  if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_COMMIT, NULL,
                                   commit, &commit_csum,
                                   cancellable, error))
    goto out;

  ret_commit = ostree_checksum_from_bytes (commit_csum);

  ret = TRUE;
  ot_transfer_out_value(out_commit, &ret_commit);
 out:
  if (now)
    g_date_time_unref (now);
  return ret;
}

static GVariant *
create_tree_variant_from_hashes (GHashTable            *file_checksums,
                                 GHashTable            *dir_contents_checksums,
                                 GHashTable            *dir_metadata_checksums)
{
  GHashTableIter hash_iter;
  gpointer key, value;
  GVariantBuilder files_builder;
  GVariantBuilder dirs_builder;
  GSList *sorted_filenames = NULL;
  GSList *iter;
  GVariant *serialized_tree;

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(say)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sayay)"));

  g_hash_table_iter_init (&hash_iter, file_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *value;

      value = g_hash_table_lookup (file_checksums, name);
      g_variant_builder_add (&files_builder, "(s@ay)", name,
                             ostree_checksum_to_bytes_v (value));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  g_hash_table_iter_init (&hash_iter, dir_metadata_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *content_checksum;
      const char *meta_checksum;

      content_checksum = g_hash_table_lookup (dir_contents_checksums, name);
      meta_checksum = g_hash_table_lookup (dir_metadata_checksums, name);

      g_variant_builder_add (&dirs_builder, "(s@ay@ay)",
                             name,
                             ostree_checksum_to_bytes_v (content_checksum),
                             ostree_checksum_to_bytes_v (meta_checksum));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  serialized_tree = g_variant_new ("(@a(say)@a(sayay))",
                                   g_variant_builder_end (&files_builder),
                                   g_variant_builder_end (&dirs_builder));
  g_variant_ref_sink (serialized_tree);

  return serialized_tree;
}

struct OstreeRepoCommitModifier {
  volatile gint refcount;

  OstreeRepoCommitModifierFlags flags;
  OstreeRepoCommitFilter filter;
  gpointer user_data;
  GDestroyNotify destroy_notify;
};

static OstreeRepoCommitFilterResult
apply_commit_filter (OstreeRepo            *self,
                     OstreeRepoCommitModifier *modifier,
                     GPtrArray                *path,
                     GFileInfo                *file_info,
                     GFileInfo               **out_modified_info)
{
  GString *path_buf;
  guint i;
  OstreeRepoCommitFilterResult result;
  GFileInfo *modified_info;

  if (modifier == NULL || modifier->filter == NULL)
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];

          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  modified_info = g_file_info_dup (file_info);
  result = modifier->filter (self, path_buf->str, modified_info, modifier->user_data);
  *out_modified_info = modified_info;

  g_string_free (path_buf, TRUE);
  return result;
}

static gboolean
write_directory_to_mtree_internal (OstreeRepo                  *self,
                                   GFile                       *dir,
                                   OstreeMutableTree           *mtree,
                                   OstreeRepoCommitModifier    *modifier,
                                   GPtrArray                   *path,
                                   GCancellable                *cancellable,
                                   GError                     **error)
{
  gboolean ret = FALSE;
  gboolean repo_dir_was_empty = FALSE;
  OstreeRepoCommitFilterResult filter_result;
  gs_unref_object OstreeRepoFile *repo_dir = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;

  g_debug ("Examining: %s", gs_file_get_path_cached (dir));

  /* We can only reuse checksums directly if there's no modifier */
  if (OSTREE_IS_REPO_FILE (dir) && modifier == NULL)
    repo_dir = (OstreeRepoFile*)g_object_ref (dir);

  if (repo_dir)
    {
      if (!ostree_repo_file_ensure_resolved (repo_dir, error))
        goto out;

      ostree_mutable_tree_set_metadata_checksum (mtree, ostree_repo_file_get_checksum (repo_dir));
      repo_dir_was_empty =
        g_hash_table_size (ostree_mutable_tree_get_files (mtree)) == 0
        && g_hash_table_size (ostree_mutable_tree_get_subdirs (mtree)) == 0;

      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      gs_unref_object GFileInfo *modified_info = NULL;
      gs_unref_variant GVariant *xattrs = NULL;
      gs_free guchar *child_file_csum = NULL;
      gs_free char *tmp_checksum = NULL;

      child_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      cancellable, error);
      if (!child_info)
        goto out;

      filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

      if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
        {
          g_debug ("Adding: %s", gs_file_get_path_cached (dir));
          if (!(modifier && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS) > 0))
            {
              if (!ostree_get_xattrs_for_file (dir, &xattrs, cancellable, error))
                goto out;
            }

          if (!_ostree_repo_write_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                                  cancellable, error))
            goto out;

          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
        }

      g_clear_object (&child_info);
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable,
                                            error);
      if (!dir_enum)
        goto out;

      while (TRUE)
        {
          GFileInfo *child_info;
          gs_unref_object GFile *child = NULL;
          gs_unref_object GFileInfo *modified_info = NULL;
          gs_unref_object OstreeMutableTree *child_mtree = NULL;
          const char *name;

          if (!gs_file_enumerator_iterate (dir_enum, &child_info, NULL,
                                           cancellable, error))
            goto out;
          if (child_info == NULL)
            break;

          name = g_file_info_get_name (child_info);
          g_ptr_array_add (path, (char*)name);
          filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

          if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
            {
              child = g_file_get_child (dir, name);

              if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
                {
                  if (!ostree_mutable_tree_ensure_dir (mtree, name, &child_mtree, error))
                    goto out;

                  if (!write_directory_to_mtree_internal (self, child, child_mtree,
                                                          modifier, path,
                                                          cancellable, error))
                    goto out;
                }
              else if (repo_dir)
                {
                  g_debug ("Adding: %s", gs_file_get_path_cached (child));
                  if (!ostree_mutable_tree_replace_file (mtree, name,
                                                         ostree_repo_file_get_checksum ((OstreeRepoFile*) child),
                                                         error))
                    goto out;
                }
              else
                {
                  guint64 file_obj_length;
                  const char *loose_checksum;
                  gs_unref_object GInputStream *file_input = NULL;
                  gs_unref_variant GVariant *xattrs = NULL;
                  gs_unref_object GInputStream *file_object_input = NULL;
                  gs_free guchar *child_file_csum = NULL;
                  gs_free char *tmp_checksum = NULL;

                  g_debug ("Adding: %s", gs_file_get_path_cached (child));
                  loose_checksum = devino_cache_lookup (self, child_info);

                  if (loose_checksum)
                    {
                      if (!ostree_mutable_tree_replace_file (mtree, name, loose_checksum,
                                                             error))
                        goto out;
                    }
                  else
                    {
                     if (g_file_info_get_file_type (modified_info) == G_FILE_TYPE_REGULAR)
                        {
                          file_input = (GInputStream*)g_file_read (child, cancellable, error);
                          if (!file_input)
                            goto out;
                        }

                      if (!(modifier && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS) > 0))
                        {
                          g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);
                          if (!ostree_get_xattrs_for_file (child, &xattrs, cancellable, error))
                            goto out;
                        }

                      if (!ostree_raw_file_to_content_stream (file_input,
                                                              modified_info, xattrs,
                                                              &file_object_input, &file_obj_length,
                                                              cancellable, error))
                        goto out;
                      if (!ostree_repo_write_content (self, NULL, file_object_input, file_obj_length,
                                                      &child_file_csum, cancellable, error))
                        goto out;

                      g_free (tmp_checksum);
                      tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
                      if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum,
                                                             error))
                        goto out;
                    }
                }

              g_ptr_array_remove_index (path, path->len - 1);
            }
        }
    }

  if (repo_dir && repo_dir_was_empty)
    ostree_mutable_tree_set_contents_checksum (mtree, ostree_repo_file_tree_get_contents_checksum (repo_dir));

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_write_directory_to_mtree:
 * @self: Repo
 * @dir: Path to a directory
 * @mtree: Overlay directory contents into this tree
 * @modifier: (allow-none): Optional modifier
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store objects for @dir and all children into the repository @self,
 * overlaying the resulting filesystem hierarchy into @mtree.
 */
gboolean
ostree_repo_write_directory_to_mtree (OstreeRepo                *self,
                                      GFile                     *dir,
                                      OstreeMutableTree         *mtree,
                                      OstreeRepoCommitModifier  *modifier,
                                      GCancellable              *cancellable,
                                      GError                   **error)
{
  gboolean ret = FALSE;
  GPtrArray *path = NULL;

  path = g_ptr_array_new ();
  if (!write_directory_to_mtree_internal (self, dir, mtree, modifier, path,
                                          cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (path)
    g_ptr_array_free (path, TRUE);
  return ret;
}

/**
 * ostree_repo_write_mtree:
 * @self: Repo
 * @mtree: Mutable tree
 * @out_contents_checksum: (out): Return location for ASCII checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write all metadata objects for @mtree to repo; the resulting
 * @out_contents_checksum contains the checksum for the
 * %OSTREE_OBJECT_TYPE_DIR_TREE object.
 */
gboolean
ostree_repo_write_mtree (OstreeRepo           *self,
                         OstreeMutableTree    *mtree,
                         char                **out_contents_checksum,
                         GCancellable         *cancellable,
                         GError              **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  const char *existing_checksum;
  gs_free char *ret_contents_checksum = NULL;
  gs_unref_hashtable GHashTable *dir_metadata_checksums = NULL;
  gs_unref_hashtable GHashTable *dir_contents_checksums = NULL;
  gs_unref_variant GVariant *serialized_tree = NULL;
  gs_free guchar *contents_csum = NULL;

  existing_checksum = ostree_mutable_tree_get_contents_checksum (mtree);
  if (existing_checksum)
    {
      ret_contents_checksum = g_strdup (existing_checksum);
    }
  else
    {
      dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);

      g_hash_table_iter_init (&hash_iter, ostree_mutable_tree_get_subdirs (mtree));
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *name = key;
          const char *metadata_checksum;
          OstreeMutableTree *child_dir = value;
          char *child_dir_contents_checksum;

          if (!ostree_repo_write_mtree (self, child_dir, &child_dir_contents_checksum,
                                        cancellable, error))
            goto out;

          g_assert (child_dir_contents_checksum);
          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                child_dir_contents_checksum); /* Transfer ownership */
          metadata_checksum = ostree_mutable_tree_get_metadata_checksum (child_dir);
          g_assert (metadata_checksum);
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (metadata_checksum));
        }

      serialized_tree = create_tree_variant_from_hashes (ostree_mutable_tree_get_files (mtree),
                                                         dir_contents_checksums,
                                                         dir_metadata_checksums);

      if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_TREE, NULL,
                                       serialized_tree, &contents_csum,
                                       cancellable, error))
        goto out;
      ret_contents_checksum = ostree_checksum_from_bytes (contents_csum);
    }

  ret = TRUE;
  ot_transfer_out_value(out_contents_checksum, &ret_contents_checksum);
 out:
  return ret;
}

/**
 * ostree_repo_commit_modifier_new:
 * @flags: Control options for filter
 * @commit_filter: (allow-none): Function that can inspect individual files
 * @user_data: (allow-none): User data
 * @destroy_notify: A #GDestroyNotify
 *
 * Returns: (transfer full): A new commit modifier.
 */
OstreeRepoCommitModifier *
ostree_repo_commit_modifier_new (OstreeRepoCommitModifierFlags  flags,
                                 OstreeRepoCommitFilter         commit_filter,
                                 gpointer                       user_data,
                                 GDestroyNotify                 destroy_notify)
{
  OstreeRepoCommitModifier *modifier = g_new0 (OstreeRepoCommitModifier, 1);

  modifier->refcount = 1;
  modifier->flags = flags;
  modifier->filter = commit_filter;
  modifier->user_data = user_data;
  modifier->destroy_notify = destroy_notify;

  return modifier;
}

OstreeRepoCommitModifier *
ostree_repo_commit_modifier_ref (OstreeRepoCommitModifier *modifier)
{
  g_atomic_int_inc (&modifier->refcount);
  return modifier;
}

void
ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier)
{
  if (!modifier)
    return;
  if (!g_atomic_int_dec_and_test (&modifier->refcount))
    return;

  if (modifier->destroy_notify)
    modifier->destroy_notify (modifier->user_data);

  g_free (modifier);
  return;
}

G_DEFINE_BOXED_TYPE(OstreeRepoCommitModifier, ostree_repo_commit_modifier,
                    ostree_repo_commit_modifier_ref,
                    ostree_repo_commit_modifier_unref);

static OstreeRepoTransactionStats *
ostree_repo_transaction_stats_copy (OstreeRepoTransactionStats *stats)
{
  return g_memdup (stats, sizeof (OstreeRepoTransactionStats));
}

static void
ostree_repo_transaction_stats_free (OstreeRepoTransactionStats *stats)
{
  return g_free (stats);
}

G_DEFINE_BOXED_TYPE(OstreeRepoTransactionStats, ostree_repo_transaction_stats,
                    ostree_repo_transaction_stats_copy,
                    ostree_repo_transaction_stats_free);