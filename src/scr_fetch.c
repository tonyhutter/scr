/*
 * Copyright (c) 2009, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Written by Adam Moody <moody20@llnl.gov>.
 * LLNL-CODE-411039.
 * All rights reserved.
 * This file is part of The Scalable Checkpoint / Restart (SCR) library.
 * For details, see https://sourceforge.net/projects/scalablecr/
 * Please also read this file: LICENSE.TXT.
*/

#include "scr_globals.h"

/*
=========================================
Fetch functions
=========================================
*/

/* Overview of fetch process:
 *   1) Read index file from prefix directory
 *   2) Find most recent complete checkpoint in index file (that we've not marked as bad)
 *   3) Exit with failure if no checkpoints remain
 *   4) Read and scatter summary file information for this checkpoint
 *   5) Copy files from checkpoint directory to cache
 *        - Flow control from rank 0 via sliding window
 *        - File data may exist as physical file on parallel file system or
 *          be encapsulated in a "container" (physical file that contains
 *          bytes for one or more application files)
 *        - Optionally check CRC32 values as files are read in
 *   6) If successful, stop, otherwise mark this checkpoint as bad and repeat #2
 */

/* for file name listed in meta, fetch that file from src_dir and store
 * a copy in dst_dir, record full path to copy in newfile, and
 * return whether operation succeeded */
static int scr_fetch_file(
  const char* src_dir, const scr_meta* meta, const char* dst_dir,
  char* newfile, size_t newfile_size)
{
  int rc = SCR_SUCCESS;

  /* get the filename from the meta data */
  char* meta_filename;
  if (scr_meta_get_filename(meta, &meta_filename) != SCR_SUCCESS) {
    scr_err("Failed to read filename from meta data @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* build full path to file */
  char filename[SCR_MAX_FILENAME];
  if (scr_build_path(filename, sizeof(filename), src_dir, meta_filename) != SCR_SUCCESS) {
    scr_err("Failed to build full file name of target file for fetch @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* fetch the file */
  uLong crc;
  uLong* crc_p = NULL;
  if (scr_crc_on_flush) {
    crc_p = &crc;
  }
  rc = scr_copy_to(filename, dst_dir, scr_file_buf_size, newfile, newfile_size, crc_p);

  /* check that crc matches crc stored in meta */
  uLong meta_crc;
  if (scr_meta_get_crc32(meta, &meta_crc) == SCR_SUCCESS) {
    if (rc == SCR_SUCCESS && scr_crc_on_flush && crc != meta_crc) {
      rc = SCR_FAILURE;
      scr_err("CRC32 mismatch detected when fetching file from %s to %s @ %s:%d",
        filename, newfile, __FILE__, __LINE__
      );

      /* TODO: would be good to log this, but right now only rank 0 can write log entries */
      /*
      if (scr_log_enable) {
        time_t now = scr_log_seconds();
        scr_log_event("CRC32 MISMATCH", filename, NULL, &now, NULL);
      }
      */
    }
  }

  return rc;
}

/* extract container name, size, offset, and length values
 * for container that holds the specified segment */
int scr_container_get_name_size_offset_length(
  const scr_hash* segment, const scr_hash* containers,
  char** name, unsigned long* size, unsigned long* offset, unsigned long* length)
{
  /* check that our parameters are valid */
  if (segment == NULL || containers == NULL ||
      name == NULL || size == NULL || offset == NULL || length == NULL)
  {
    return SCR_FAILURE;
  }

  /* lookup the segment length */
  if (scr_hash_util_get_bytecount(segment, SCR_SUMMARY_6_KEY_LENGTH, length) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  /* get the container hash */
  scr_hash* container = scr_hash_get(segment, SCR_SUMMARY_6_KEY_CONTAINER);

  /* lookup id for container */
  int id;
  if (scr_hash_util_get_int(container, SCR_SUMMARY_6_KEY_ID, &id) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  /* lookup the offset value */
  if (scr_hash_util_get_bytecount(container, SCR_SUMMARY_6_KEY_OFFSET, offset) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  /* get container with matching id from containers list */
  scr_hash* info = scr_hash_getf(containers, "%d", id);

  /* get name of container */
  if (scr_hash_util_get_str(info, SCR_KEY_NAME, name) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  /* get size of container */
  if (scr_hash_util_get_bytecount(info, SCR_KEY_SIZE, size) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  return SCR_SUCCESS;
}

/* fetch file in meta from its list of segments and containers and
 * write it to specified file name, return whether operation succeeded */
static int scr_fetch_file_from_containers(const char* file, scr_meta* meta, scr_hash* segments, const scr_hash* containers)
{
  unsigned long buf_size = scr_file_buf_size;

  /* check that we got something for a source file */
  if (file == NULL || strcmp(file, "") == 0) {
    scr_err("Invalid source file @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* check that our other arguments are valid */
  if (meta == NULL || segments == NULL || containers == NULL) {
    scr_err("Invalid metadata, segments, or container @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* open the file for writing */
  int fd_src = scr_open(file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd_src < 0) {
    scr_err("Opening file to copy: scr_open(%s) errno=%d %m @ %s:%d",
      file, errno, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* TODO:
  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED | POSIX_FADV_SEQUENTIAL)
  that tells the kernel that you don't ever need the pages
  from the file again, and it won't bother keeping them in the page cache.
  */
  posix_fadvise(fd_src, 0, 0, POSIX_FADV_DONTNEED | POSIX_FADV_SEQUENTIAL);

  /* TODO: align this buffer */
  /* allocate buffer to read in file chunks */
  char* buf = (char*) malloc(buf_size);
  if (buf == NULL) {
    scr_err("Allocating memory: malloc(%llu) errno=%d %m @ %s:%d",
      buf_size, errno, __FILE__, __LINE__
    );
    scr_close(file, fd_src);
    return SCR_FAILURE;
  }

  /* initialize crc value */
  uLong crc;
  if (scr_crc_on_flush) {
    crc = crc32(0L, Z_NULL, 0);
  }

  int rc = SCR_SUCCESS;

  /* read in each segment */
  scr_hash_sort_int(segments, SCR_HASH_SORT_ASCENDING);
  scr_hash_elem* elem;
  for (elem = scr_hash_elem_first(segments);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get the container info for this segment */
    scr_hash* hash = scr_hash_elem_hash(elem);

    /* get the offset into the container and the length of the segment (both in bytes) */
    char* container_name;
    unsigned long container_size, container_offset, segment_length;
    if (scr_container_get_name_size_offset_length(hash, containers,
      &container_name, &container_size, &container_offset, &segment_length) != SCR_SUCCESS)
    {
      scr_err("Failed to get segment offset and length @ %s:%d",
        __FILE__, __LINE__
      );
      rc = SCR_FAILURE;
      break;
    }

    /* open container file for reading */
    int fd_container = scr_open(container_name, O_RDONLY);
    if (fd_container < 0) {
      scr_err("Opening file for reading: scr_open(%s) errno=%d %m @ %s:%d",
        container_name, errno, __FILE__, __LINE__
      );
      rc = SCR_FAILURE;
      break;
    }

    /* TODO:
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED | POSIX_FADV_SEQUENTIAL)
    that tells the kernel that you don't ever need the pages
    from the file again, and it won't bother keeping them in the page cache.
    */
    posix_fadvise(fd_container, 0, 0, POSIX_FADV_DONTNEED | POSIX_FADV_SEQUENTIAL);

    /* seek to offset within container */
    off_t pos = (off_t) container_offset;
    if (lseek(fd_container, pos, SEEK_SET) == (off_t)-1) {
      /* our seek failed, return an error */
      scr_err("Failed to seek to byte %lu in %s @ %s:%d",
        pos, container_name, __FILE__, __LINE__
      );
      rc = SCR_FAILURE;
      break;
    }
    
    /* copy data from container into file in chunks */
    unsigned long remaining = segment_length;
    while (remaining > 0) {
      /* read / write up to buf_size bytes at a time from file */
      unsigned long count = remaining;
      if (count > buf_size) {
        count = buf_size;
      }

      /* attempt to read buf_size bytes from container */
      int nread = scr_read_attempt(container_name, fd_container, buf, count);

      /* if we read some bytes, write them out */
      if (nread > 0) {
        /* optionally compute crc value as we go */
        if (scr_crc_on_flush) {
          crc = crc32(crc, (const Bytef*) buf, (uInt) nread);
        }

        /* write our nread bytes out */
        int nwrite = scr_write_attempt(file, fd_src, buf, nread);

        /* check for a write error or a short write */
        if (nwrite != nread) {
          /* write had a problem, stop copying and return an error */
          rc = SCR_FAILURE;
          break;
        }

        /* subtract the bytes we've processed from the number remaining */
        remaining -= (unsigned long) nread;
      }

      /* assume a short read is an error */
      if (nread < count) {
        /* read had a problem, stop copying and return an error */
        rc = SCR_FAILURE;
        break;
      }

      /* check for a read error, stop copying and return an error */
      if (nread < 0) {
        /* read had a problem, stop copying and return an error */
        rc = SCR_FAILURE;
        break;
      }
    }

    /* close container */
    if (scr_close(container_name, fd_container) != SCR_SUCCESS) {
      rc = SCR_FAILURE;
    }
  }

  /* close the source file */
  if (scr_close(file, fd_src) != SCR_SUCCESS) {
    rc = SCR_FAILURE;
  }

  /* free buffer */
  scr_free(&buf);

  /* verify crc value */
  if (rc == SCR_SUCCESS) {
    uLong crc2;
    if (scr_crc_on_flush) {
      if (scr_meta_get_crc32(meta, &crc2) == SCR_SUCCESS) {
        /* if a crc is already set in the meta data, check that we computed the same value */
        if (crc != crc2) {
          scr_err("CRC32 mismatch detected when fetching file %s @ %s:%d",
            file, __FILE__, __LINE__
          );
          rc = SCR_FAILURE;
        }
      }
    }
  }

  return rc;
}

/* fetch files listed in hash into specified cache directory,
 * update filemap and fill in total number of bytes fetched,
 * returns SCR_SUCCESS if successful */
static int scr_fetch_files_list(const scr_hash* file_list, const char* dir, scr_filemap* map)
{
  /* assume we'll succeed in fetching our files */
  int rc = SCR_SUCCESS;

  /* assume we don't have any files to fetch */
  int my_num_files = 0;

  /* get dataset id */
  int id;
  scr_dataset* dataset = scr_hash_get(file_list, SCR_KEY_DATASET);
  scr_dataset_get_id(dataset, &id);

  /* get pointer to containers hash */
  scr_hash* containers = scr_hash_get(file_list, SCR_SUMMARY_6_KEY_CONTAINER);

  /* now iterate through the file list and fetch each file */
  scr_hash_elem* file_elem = NULL;
  scr_hash* files = scr_hash_get(file_list, SCR_KEY_FILE);
  for (file_elem = scr_hash_elem_first(files);
       file_elem != NULL;
       file_elem = scr_hash_elem_next(file_elem))
  {
    /* get the filename */
    char* file = scr_hash_elem_key(file_elem);

    /* get a pointer to the hash for this file */
    scr_hash* hash = scr_hash_elem_hash(file_elem);

    /* check whether we are supposed to fetch this file */
    /* TODO: this is a hacky way to avoid reading a redundancy file back in
     * under the assumption that it's an original file, which breaks our
     * redundancy computation due to a name conflict on the file names */
    scr_hash_elem* no_fetch_hash = scr_hash_elem_get(hash, SCR_SUMMARY_6_KEY_NOFETCH);
    if (no_fetch_hash != NULL) {
      continue;
    }

    /* increment our file count */
    my_num_files++;

    /* split filename into path and name components */
    char path[SCR_MAX_FILENAME];
    char name[SCR_MAX_FILENAME];
    scr_split_path(file, path, name);

    /* build the destination file name */
    char newfile[SCR_MAX_FILENAME];
    scr_build_path(newfile, sizeof(newfile), dir, name);
      
    /* add the file to our filemap and write it to disk before creating the file,
     * this way we have a record that it may exist before we actually start to fetch it */
    scr_filemap_add_file(map, id, scr_my_rank_world, newfile);
    scr_filemap_write(scr_map_file, map);

    /* get the file size */
    unsigned long filesize = 0;
    if (scr_hash_util_get_unsigned_long(hash, SCR_KEY_SIZE, &filesize) != SCR_SUCCESS) {
      scr_err("Failed to read file size from summary data @ %s:%d",
        __FILE__, __LINE__
      );
      rc = SCR_FAILURE;
      break;
    }

    /* check for a complete flag */
    int complete = 1;
    if (scr_hash_util_get_int(hash, SCR_KEY_COMPLETE, &complete) != SCR_SUCCESS) {
      /* in summary file, the absence of a complete flag on a file implies the file is complete */
      complete = 1;
    }

    /* create a new meta data object for this file */
    scr_meta* meta = scr_meta_new();

    /* set the meta data */
    scr_meta_set_filename(meta, newfile);
    scr_meta_set_filetype(meta, SCR_META_FILE_FULL);
    scr_meta_set_filesize(meta, filesize);
    scr_meta_set_complete(meta, 1);
    /* TODODSET: move the ranks field elsewhere, for now it's needed by scr_index.c */
    scr_meta_set_ranks(meta, scr_ranks_world);

    /* get the crc, if set, and add it to the meta data */
    uLong crc;
    if (scr_hash_util_get_crc32(hash, SCR_KEY_CRC, &crc) == SCR_SUCCESS) {
      scr_meta_set_crc32(meta, crc);
    }

    /* fetch file from containers if they are defined, otherwise fetch the native file */
    if (containers != NULL) {
      /* lookup segments hash for this file */
      scr_hash* segments = scr_hash_get(hash, SCR_SUMMARY_6_KEY_SEGMENT);

      /* fetch file from containers */
      if (scr_fetch_file_from_containers(newfile, meta, segments, containers) != SCR_SUCCESS) {
        /* failed to fetch file, mark it as incomplete */
        scr_meta_set_complete(meta, 0);
        rc = SCR_FAILURE;
      }
    } else {
      /* fetch native file, lookup directory for this file */
      char* from_dir;
      if (scr_hash_util_get_str(hash, SCR_KEY_PATH, &from_dir) == SCR_SUCCESS) {
        if (scr_fetch_file(from_dir, meta, dir, newfile, sizeof(newfile)) != SCR_SUCCESS) {
          /* failed to fetch file, mark it as incomplete */
          scr_meta_set_complete(meta, 0);
          rc = SCR_FAILURE;
        }
      } else {
        /* failed to read source directory, mark file as incomplete */
        scr_meta_set_complete(meta, 0);
        rc = SCR_FAILURE;
      }
    }

    /* TODODSET: want to write out filemap before we start to fetch each file? */

    /* mark the file as complete */
    scr_filemap_set_meta(map, id, scr_my_rank_world, newfile, meta);

    /* free the meta data object */
    scr_meta_delete(meta);
  }

  /* set the expected number of files for this dataset */
  scr_filemap_set_expected_files(map, id, scr_my_rank_world, my_num_files);
  scr_filemap_write(scr_map_file, map);

  return rc;
}

/* read contents of summary file */
static int scr_fetch_summary(const char* dir, scr_hash* file_list)
{
  /* assume that we won't succeed in our fetch attempt */
  int rc = SCR_FAILURE;

  /* get a new hash to read summary data into */
  scr_hash* summary_hash = scr_hash_new();

  /* have rank 0 read summary file, if it exists */
  if (scr_my_rank_world == 0) {
    /* check that we can access the directory */
    if (scr_file_is_readable(dir) == SCR_SUCCESS) {
      /* read data from the summary file */
      rc = scr_summary_read(dir, summary_hash);
    } else {
      scr_err("Failed to access directory %s @ %s:%d",
        dir, __FILE__, __LINE__
      );
    }
  }

  /* broadcast success code from rank 0 */
  MPI_Bcast(&rc, 1, MPI_INT, 0, scr_comm_world);

  /* scatter data from summary file to other ranks */
  if (rc == SCR_SUCCESS) {
    /* broadcast the dataset information */
    scr_hash* dataset_hash = scr_hash_new();
    if (scr_my_rank_world == 0) {
      scr_dataset* dataset = scr_hash_get(summary_hash, SCR_SUMMARY_6_KEY_DATASET);
      scr_hash_merge(dataset_hash, dataset);
    }
    scr_hash_bcast(dataset_hash, 0, scr_comm_world);
    scr_hash_set(file_list, SCR_SUMMARY_6_KEY_DATASET, dataset_hash);

    /* TODO: it's overkill to bcast info for all containers, each proc only really needs to know
     * about the containers that contain its files */

    /* broadcast the container file information if we have any */
    scr_hash* container_hash = scr_hash_new();
    if (scr_my_rank_world == 0) {
      scr_dataset* container = scr_hash_get(summary_hash, SCR_SUMMARY_6_KEY_CONTAINER);
      scr_hash_merge(container_hash, container);
    }
    scr_hash_bcast(container_hash, 0, scr_comm_world);
    if (scr_hash_size(container_hash) > 0) {
      scr_hash_set(file_list, SCR_SUMMARY_6_KEY_CONTAINER, container_hash);
    } else {
      scr_hash_delete(container_hash);
    }

    /* scatter out file information for each rank */
    scr_hash* send_hash = NULL;
    if (scr_my_rank_world == 0) {
      scr_hash* rank2file_hash = scr_hash_get(summary_hash, SCR_SUMMARY_6_KEY_RANK2FILE);
      send_hash = scr_hash_get(rank2file_hash, SCR_SUMMARY_6_KEY_RANK);
    }
    scr_hash* recv_hash = scr_hash_new();
    scr_hash_exchange(send_hash, recv_hash, scr_comm_world);

    /* iterate over the ranks that sent data to us, and set up our list of files */
    scr_hash_elem* elem;
    for (elem = scr_hash_elem_first(recv_hash);
         elem != NULL;
         elem = scr_hash_elem_next(elem))
    {
      /* get the file hash from the current rank */
      scr_hash* elem_hash = scr_hash_elem_hash(elem);
      scr_hash* file_hash = scr_hash_get(elem_hash, SCR_SUMMARY_6_KEY_FILE);

      /* copy the file hash */
      scr_hash* tmp_hash = scr_hash_new();
      scr_hash_merge(tmp_hash, file_hash);
      scr_hash_set(file_list, SCR_KEY_FILE, tmp_hash);
    }
    scr_hash_delete(recv_hash);

    /* if we're not using containers, add PATH entry for each of our files */
    scr_hash* files = scr_hash_get(file_list, SCR_KEY_FILE);
    for (elem = scr_hash_elem_first(files);
         elem != NULL;
         elem = scr_hash_elem_next(elem))
    {
      /* set the source path of each file */
      scr_hash* hash = scr_hash_elem_hash(elem);
      scr_hash_util_set_str(hash, SCR_KEY_PATH, dir);
    }
  }

  /* delete the summary hash object */
  scr_hash_delete(summary_hash);

  return rc;
}

/* fetch files specified in file_list into specified dir and update filemap */
static int scr_fetch_data(const scr_hash* file_list, const char* dir, scr_filemap* map)
{
  int success = SCR_SUCCESS;

  /* flow control rate of file reads from rank 0 */
  if (scr_my_rank_world == 0) {
    /* fetch these files into the directory */
    if (scr_fetch_files_list(file_list, dir, map) != SCR_SUCCESS) {
      success = SCR_FAILURE;
    }

    /* now, have a sliding window of w processes read simultaneously */
    int w = scr_fetch_width;
    if (w > scr_ranks_world-1) {
      w = scr_ranks_world-1;
    }

    /* allocate MPI_Request arrays and an array of ints */
    int* flags       = (int*)         malloc(2 * w * sizeof(int));
    MPI_Request* req = (MPI_Request*) malloc(2 * w * sizeof(MPI_Request));
    MPI_Status status;
    if (flags == NULL || req == NULL) {
      scr_abort(-1, "Failed to allocate memory for flow control @ %s:%d",
        __FILE__, __LINE__
      );
    }

    /* execute our flow control window */
    int outstanding = 0;
    int index = 0;
    int i = 1;
    while (i < scr_ranks_world || outstanding > 0) {
      /* issue up to w outstanding sends and receives */
      while (i < scr_ranks_world && outstanding < w) {
        /* post a receive for the response message we'll get back when rank i is done */
        MPI_Irecv(&flags[index + w], 1, MPI_INT, i, 0, scr_comm_world, &req[index + w]);

        /* send a start signal to this rank */
        flags[index] = success;
        MPI_Isend(&flags[index], 1, MPI_INT, i, 0, scr_comm_world, &req[index]);

        /* update the number of outstanding requests */
        outstanding++;
        index++;
        i++;
      }

      /* wait to hear back from any rank */
      MPI_Waitany(w, &req[w], &index, &status);

      /* the corresponding send must be complete */
      MPI_Wait(&req[index], &status);

      /* check success code from process */
      if (flags[index + w] != SCR_SUCCESS) {
        success = SCR_FAILURE;
      }

      /* one less request outstanding now */
      outstanding--;
    }

    /* free the MPI_Request arrays */
    scr_free(&req);
    scr_free(&flags);
  } else {
    /* wait for start signal from rank 0 */
    MPI_Status status;
    MPI_Recv(&success, 1, MPI_INT, 0, 0, scr_comm_world, &status);

    /* if rank 0 hasn't seen a failure, try to read in our files */
    if (success == SCR_SUCCESS) {
      /* fetch these files into the directory */
      if (scr_fetch_files_list(file_list, dir, map) != SCR_SUCCESS) {
        success = SCR_FAILURE;
      }
    }

    /* tell rank 0 that we're done and send him our success code */
    MPI_Send(&success, 1, MPI_INT, 0, 0, scr_comm_world);
  }

  /* determine whether all processes successfully read their files */
  if (scr_alltrue(success == SCR_SUCCESS)) {
    return SCR_SUCCESS;
  }
  return SCR_FAILURE;
}

/* fetch files from parallel file system */
static int scr_fetch_files(scr_filemap* map, char* fetch_dir, int* dataset_id, int* checkpoint_id)
{
  /* this may take a while, so tell user what we're doing */
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "Attempting fetch from %s", fetch_dir);
  }

  /* make sure all processes make it this far before progressing */
  MPI_Barrier(scr_comm_world);

  /* start timer */
  time_t timestamp_start;
  double time_start;
  if (scr_my_rank_world == 0) {
    timestamp_start = scr_log_seconds();
    time_start = MPI_Wtime();
  }

  /* broadcast fetch directory */
  int dirsize = 0;
  if (scr_my_rank_world == 0) {
    dirsize = strlen(fetch_dir) + 1;
  }
  MPI_Bcast(&dirsize, 1, MPI_INT, 0, scr_comm_world);
  MPI_Bcast(fetch_dir, dirsize, MPI_CHAR, 0, scr_comm_world);

  /* if there is no directory, bail out with failure */
  if (strcmp(fetch_dir, "") == 0) {
    return SCR_FAILURE;
  }

  /* log the fetch attempt */
  if (scr_my_rank_world == 0) {
    if (scr_log_enable) {
      time_t now = scr_log_seconds();
      scr_log_event("FETCH STARTED", fetch_dir, NULL, &now, NULL);
    }
  }

  /* allocate a new hash to get a list of files to fetch */
  scr_hash* file_list = scr_hash_new();

  /* read the summary file */
  if (scr_fetch_summary(fetch_dir, file_list) != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "Failed to read summary file @ %s:%d", __FILE__, __LINE__);
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FETCH FAILED", fetch_dir, NULL, &now, &time_diff);
      }
    }
    scr_hash_delete(file_list);
    return SCR_FAILURE;
  }

  /* get a pointer to the dataset */
  scr_dataset* dataset = scr_hash_get(file_list, SCR_KEY_DATASET);

  /* get the dataset id */
  int id;
  if (scr_dataset_get_id(dataset, &id) != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "Invalid id in summary file @ %s:%d", __FILE__, __LINE__);
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FETCH FAILED", fetch_dir, NULL, &now, &time_diff);
      }
    }
    scr_hash_delete(file_list);
    return SCR_FAILURE;
  }

  /* get the checkpoint id for this dataset */
  int ckpt_id;
  if (scr_dataset_get_ckpt(dataset, &ckpt_id) != SCR_SUCCESS) {
    /* eventually, we'll support reading of non-checkpoint datasets, but we don't yet */
    scr_err("Failed to read checkpoint id from dataset @ %s:%d",
      __FILE__, __LINE__
    );
    scr_hash_delete(file_list);
    return SCR_FAILURE;
  }

  /* delete any existing files for this dataset id (do this before filemap_read) */
  scr_cache_delete(map, id);

  /* get the redundancy descriptor for this id */
  scr_reddesc* c = scr_reddesc_for_checkpoint(ckpt_id, scr_nreddescs, scr_reddescs);

  /* store our redundancy descriptor hash in the filemap */
  scr_hash* my_desc_hash = scr_hash_new();
  scr_reddesc_store_to_hash(c, my_desc_hash);
  scr_filemap_set_desc(map, id, scr_my_rank_world, my_desc_hash);
  scr_hash_delete(my_desc_hash);

  /* write the filemap out before creating the directory */
  scr_filemap_write(scr_map_file, map);

  /* create the cache directory */
  scr_cache_dir_create(c, id);

  /* get the cache directory */
  char cache_dir[SCR_MAX_FILENAME];
  scr_cache_dir_get(c, id, cache_dir);

  /* now we can finally fetch the actual files */
  int success = 1;
  if (scr_fetch_data(file_list, cache_dir, map) != SCR_SUCCESS) {
    success = 0;
  }

  /* free the hash holding the summary file data */
  scr_hash_delete(file_list);

  /* check that all processes copied their file successfully */
  if (! scr_alltrue(success)) {
    /* someone failed, so let's delete the partial checkpoint */
    scr_cache_delete(map, id);

    if (scr_my_rank_world == 0) {
      scr_dbg(1, "One or more processes failed to read its files @ %s:%d",
        __FILE__, __LINE__
      );
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FETCH FAILED", fetch_dir, &id, &now, &time_diff);
      }
    }
    return SCR_FAILURE;
  }

  /* apply redundancy scheme */
  double bytes_copied = 0.0;
  int rc = scr_reddesc_apply(map, c, id, &bytes_copied);
  if (rc == SCR_SUCCESS) {
    /* record dataset and checkpoint ids */
    *dataset_id = id;
    *checkpoint_id = ckpt_id;

    /* update our flush file to indicate this checkpoint is in cache as well as the parallel file system */
    /* TODO: should we place SCR_FLUSH_KEY_LOCATION_PFS before scr_reddesc_apply? */
    scr_flush_file_location_set(id, SCR_FLUSH_KEY_LOCATION_CACHE);
    scr_flush_file_location_set(id, SCR_FLUSH_KEY_LOCATION_PFS);
    scr_flush_file_location_unset(id, SCR_FLUSH_KEY_LOCATION_FLUSHING);
  } else {
    /* something went wrong, so delete this checkpoint from the cache */
    scr_cache_delete(scr_map, id);
  }

  /* stop timer, compute bandwidth, and report performance */
  double total_bytes = bytes_copied;
  if (scr_my_rank_world == 0) {
    double time_end = MPI_Wtime();
    double time_diff = time_end - time_start;
    double bw = total_bytes / (1024.0 * 1024.0 * time_diff);
    scr_dbg(1, "scr_fetch_files: %f secs, %e bytes, %f MB/s, %f MB/s per proc",
      time_diff, total_bytes, bw, bw/scr_ranks_world
    );

    /* log data on the fetch to the database */
    if (scr_log_enable) {
      time_t now = scr_log_seconds();
      if (rc == SCR_SUCCESS) {
        scr_log_event("FETCH SUCCEEDED", fetch_dir, &id, &now, &time_diff);
      } else {
        scr_log_event("FETCH FAILED", fetch_dir, &id, &now, &time_diff);
      }

      char cache_dir[SCR_MAX_FILENAME];
      scr_cache_dir_get(c, id, cache_dir);
      scr_log_transfer("FETCH", fetch_dir, cache_dir, &id,
        &timestamp_start, &time_diff, &total_bytes
      );
    }
  }

  return rc;
}

/* attempt to fetch most recent checkpoint from prefix directory into cache,
 * fills in map if successful and sets fetch_attempted to 1 if any fetch is attempted,
 * returns SCR_SUCCESS if successful */
int scr_fetch_sync(scr_filemap* map, int* fetch_attempted)
{
  /* we only return success if we successfully fetch a checkpoint */
  int rc = SCR_FAILURE;

  double time_start, time_end, time_diff;

  /* start timer */
  if (scr_my_rank_world == 0) {
    time_start = MPI_Wtime();
  }

  /* build the filename for the current symlink */
  char scr_current[SCR_MAX_FILENAME];
  scr_build_path(scr_current, sizeof(scr_current), scr_par_prefix, SCR_CURRENT_LINK);

  /* have rank 0 read the index file */
  scr_hash* index_hash = NULL;
  int read_index_file = 0;
  if (scr_my_rank_world == 0) {
    /* create an empty hash to store our index */
    index_hash = scr_hash_new();

    /* read the index file */
    if (scr_index_read(scr_par_prefix, index_hash) == SCR_SUCCESS) {
      /* remember that we read the index file ok, so we know we can write to it later
       * this way we don't overwrite an existing index file just because the read happened to fail */
      read_index_file = 1;
    }
  }

  /* now start fetching, we keep trying until we exhaust all valid checkpoints */
  char target[SCR_MAX_FILENAME];
  char fetch_dir[SCR_MAX_FILENAME];
  int current_checkpoint_id = -1;
  int continue_fetching = 1;
  while (continue_fetching) {
    /* initialize our target and fetch directory values to empty strings */
    strcpy(target, "");
    strcpy(fetch_dir, "");

    /* rank 0 determines the directory to fetch from */
    if (scr_my_rank_world == 0) {
      /* read the target of the current symlink if there is one */
      if (scr_file_is_readable(scr_current) == SCR_SUCCESS) {
        int target_len = readlink(scr_current, target, sizeof(target)-1);
        if (target_len >= 0) {
          target[target_len] = '\0';
        }
      }

      /* if we read the index file, lookup the checkpoint id */
      if (read_index_file) {
        int next_checkpoint_id = -1;
        if (strcmp(target, "") != 0) {
          /* we have a subdirectory name, lookup the checkpoint id corresponding to this directory */
          scr_index_get_id_by_dir(index_hash, target, &next_checkpoint_id);
        } else {
          /* otherwise, just get the most recent complete checkpoint
           * (that's older than the current id) */
          scr_index_get_most_recent_complete(index_hash, current_checkpoint_id, &next_checkpoint_id, target);
        }
        current_checkpoint_id = next_checkpoint_id;

        /* TODODSET: need to verify that dataset is really a checkpoint and keep searching if not */
      }

      /* if we have a subdirectory (target) name, build the full fetch directory */
      if (strcmp(target, "") != 0) {
        /* record that we're attempting a fetch of this checkpoint in the index file */
        *fetch_attempted = 1;
        if (read_index_file && current_checkpoint_id != -1) {
          scr_index_mark_fetched(index_hash, current_checkpoint_id, target);
          scr_index_write(scr_par_prefix, index_hash);
        }

        /* we have a subdirectory, now build the full path */
        scr_build_path(fetch_dir, sizeof(fetch_dir), scr_par_prefix, target);
      }
    }

    /* now attempt to fetch the checkpoint */
    int dset_id, ckpt_id;
    rc = scr_fetch_files(map, fetch_dir, &dset_id, &ckpt_id);
    if (rc == SCR_SUCCESS) {
      /* set the dataset and checkpoint ids */
      scr_dataset_id = dset_id;
      scr_checkpoint_id = ckpt_id;

      /* we succeeded in fetching this checkpoint, set current to point to it, and stop fetching */
      if (scr_my_rank_world == 0) {
        symlink(target, scr_current);
      }
      continue_fetching = 0;
    } else {
      /* fetch failed, delete the current symlink */
      scr_file_unlink(scr_current);

      /* if we had a fetch directory, mark it as failed in the index file so we don't try it again */
      if (strcmp(fetch_dir, "") != 0) {
        if (scr_my_rank_world == 0) {
          if (read_index_file && current_checkpoint_id != -1 && strcmp(target, "") != 0) {
            scr_index_mark_failed(index_hash, current_checkpoint_id, target);
            scr_index_write(scr_par_prefix, index_hash);
          }
        }
      } else {
        /* we ran out of valid checkpoints in the index file, bail out of the loop */
        continue_fetching = 0;
      }
    }
  }

  /* delete the index hash */
  if (scr_my_rank_world == 0) {
    scr_hash_delete(index_hash);
  }

  /* broadcast whether we actually attempted to fetch anything (only rank 0 knows) */
  MPI_Bcast(fetch_attempted, 1, MPI_INT, 0, scr_comm_world);

  /* stop timer for fetch */
  if (scr_my_rank_world == 0) {
    time_end = MPI_Wtime();
    time_diff = time_end - time_start;
    scr_dbg(1, "scr_fetch_files: return code %d, %f secs", rc, time_diff);
  }

  return rc;
}