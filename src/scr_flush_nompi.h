#ifndef SCR_FLUSH_FILE_H
#define SCR_FLUSH_FILE_H
#include <kvtree.h>
#include "scr_dataset.h"

void scr_flush_file_location_unset_with_path(int id, const char* location,
  char* flush_file_path);

/* write summary file for flush */
int scr_flush_summary_file( const scr_dataset* dataset, int complete,
  char* summary_file);

#endif
