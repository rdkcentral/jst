/*
 If not stated otherwise in this file or this component's Licenses.txt file the
 following copyright and licenses apply:

 Copyright 2018 RDK Management

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include "jst_internal.h"

#define POST_DATA_DIR         "/tmp"      /* directory where post data is saved to disk */
#define POST_FILE_PREFIX      "jst_post_" /* prefix for each post data file saved to disk */
#define POST_FILE_TEMPLATE    POST_DATA_DIR "/" POST_FILE_PREFIX "XXXXXX" 
#define MEGABYTES(N)          ((N) * 1048576)
#define POST_MAX_SIZE         MEGABYTES(8) /* maximum post data size we allow per request */
#define POST_MAX_FILESIZE     MEGABYTES(2) /* maximum size of a single file being uploaded */
#define POST_MAX_DISK_SPACE   MEGABYTES(8) /* maximum disk space we can use to save post data */
#define TVSPEC_TO_SECONDS(t)  ((double)(t).tv_sec + ((t).tv_nsec / 1000000000.0))

/*Debug controls*/
/*set to 1 to save the post data sent via cgi to jst*/
#define DEBUG_POST_SAVE 1

/*set to 1 to load previously saved post data without needing cgi
  with this you can run jst on the command line, outside of cgi
  and make it behave as if it was in cgi mode.
  Example (after first capturing /tmp/postFile and /tmp/penvFile):
  source /tmp/penvFile
  cd $webui_jst_folder
  $jst_bin_folder/jst foo.jst
  or
  valgrind --leak-check=full --show-leak-kinds=all $jst_bin_folder/jst foo.jst
  or
  gdb --args $jst_bin_folder/jst foo.jst
*/
#define DEBUG_POST_LOAD 0

typedef enum HeaderContentType_
{
  HeaderContentTypeNull,
  HeaderContentTypeTextPlain,
  HeaderContentTypeMPFD
} HeaderContentType;

typedef enum MPFDContentType_
{
  MPFDContentTypeNull,
  MPFDContentTypeTextPlain,
  MPFDContentTypeOctetStream
} MPFDContentType;

/*these map to php error but we only support a few*/
typedef enum UploadeErr_
{
  UploadeErrOK,/*
  UploadeErrIniSize,
  UploadeErrFormSize,
  UploadeErrPartial,*/
  UploadeErrNoFile = 4,
  UploadeErrNoTmpDir,
  UploadeErrFailedWrite/*,
  UploadeErrExtension*/
} UploadeErr;

typedef struct MPFDPart_
{
  MPFDContentType type;
  char* stype;
  char* name;
  char* file_name;
  char* body;
  int body_len;
  int file_error;
  char* tmp_file_name;
} MPFDPart;

#ifdef MULTI_FILE_UPLOAD_SUPPORT
typedef struct PostFileStat_
{
  char* path;
  double age;
  unsigned long long size;
} PostFileStat;
#endif

static char* post_data = NULL;
static char* files_data = NULL;
static int file_count = 0;
extern const char* jst_debug_file_name;

static duk_ret_t get_post(duk_context *ctx)
{
  if(post_data)
  {
    duk_push_string(ctx, post_data); 
    if(post_data != NULL){
	free(post_data); //freeing here is ok only because jst_prefix.js calls getPost once	
    }
    post_data = NULL;
    return 1;
  }
  else
  {
    RETURN_FALSE;
  }
}

static duk_ret_t get_files(duk_context *ctx)
{
  if(files_data)
  {
    duk_push_string(ctx, files_data);
    free(files_data); //freeing here is ok only because jst_prefix.js calls getFiles once
    files_data = NULL;
    return 1;
  }
  else
  {
    RETURN_FALSE;
  }
}

static const duk_function_list_entry ccsp_post_funcs[] = {
  { "getPost", get_post, 0 },
  { "getFiles", get_files, 0 },
  { NULL, NULL, 0 }
};

/* In most cases post data looks like url parameters
   where you get a list of name=value pairs separated by &, eg:
    name=foo&age=10&color=red
  If header content-type is multipart/form-data we have to do special
  parsing of the post data: see
    https://datatracker.ietf.org/doc/html/rfc1867
    https://datatracker.ietf.org/doc/html/rfc2045
    https://datatracker.ietf.org/doc/html/rfc2046
*/
static int parse_content_type_header(char** boundary, int* boundary_len)
{
  const char* stype;
  int type = HeaderContentTypeTextPlain;

  *boundary = NULL;
  *boundary_len = 0;

  stype = getenv("CONTENT_TYPE");

  if(stype && strstr(stype, "multipart/form-data"))
  {
    char* boundary_start = NULL;
    type = HeaderContentTypeMPFD;

    boundary_start = strstr(stype, "boundary");
    if(boundary_start)
    {
      boundary_start = strstr(boundary_start, "=");
      if(boundary_start)
      {
        char* boundary_end = NULL;
        int len;
        boundary_start++;
        if(boundary_start[0] == '"')
        {
          boundary_start++;
          boundary_end = strchr( boundary_start, '\"');
          if(!boundary_end)
          {
            //TODO: Log parse error
            return HeaderContentTypeNull;
          }
        }
        else
        {
          boundary_end = strpbrk( boundary_start, ",;");
        }
        if(boundary_end)
          len = boundary_end - boundary_start;
        else
          len = strlen(boundary_start);

        *boundary = malloc(len + 3);
        if(!*boundary)
        {
          CosaPhpExtLog("failed to allocate boundary\n");
          return HeaderContentTypeNull;
        }
        strcpy(*boundary, "--");
        strncat(*boundary, boundary_start, len);
        *boundary_len = len + 2;
      }
    }
  }
  return type;
}

/* Example: name="file"; filename="mrollinssavedconfig.CF2" */
static int parse_name_value_pair(char** data, char* eol, char** name, char** value)
{
  char* p = *data;
  if(!p)
    return -1;
  *name = NULL;
  *value = NULL;
  while(p < eol && isspace(*p))
    p++;
  if(p < eol)
  {
    *name = p;
    while(p < eol && *p != '=')
      p++;
    if(p < eol)
      *p = 0;
    p++;
    while(p < eol && isspace(*p))
      p++;
    if(p < eol)
    {
      if(*p=='"')
      {
        p++;
        *value = p;
        while(p < eol && *p != '"')
          p++;
        if(p < eol)
          *p = 0;
        else
        {
          CosaPhpExtLog("%s: parser error missing quote\n", __FUNCTION__);
        }
      }
      else
      {
        while(p < eol && *p != ';' && !isspace(*p))
          p++;
        if(p < eol)
          *p = 0;
      }
    }
  }
  if(*name && *value)
  {
    while(p < eol && *p != ';')
      p++;
    if(p < eol)
      *data = p+1;
    else
      *data = NULL;
    return 0;
  }
  else
  {
    *data = NULL;
    return -1;
  }
}

static void init_mpfd_part(MPFDPart* part)
{
  memset(part, 0, sizeof(MPFDPart));
  part->type = MPFDContentTypeTextPlain;
}

#ifdef MULTI_FILE_UPLOAD_SUPPORT /*this needs testing*/
static int post_files_stat(PostFileStat** pfiles)
{
  DIR* dirp;
  struct dirent *dir;
  size_t prefix_len;
  int num_files;
  int capacity;
  PostFileStat* files;
  struct timespec ts_now;
  double now;

  dirp = opendir(POST_DATA_DIR);
  if(!dirp)
  {
    CosaPhpExtLog("failed to read post files directory %s: %s", POST_DATA_DIR, strerror(errno));
    return -1;
  }

  prefix_len = strlen(POST_FILE_PREFIX);

  files = NULL;
    
  timespec_get(&ts_now, TIME_UTC);
  now = TVSPEC_TO_SECONDS(ts_now);

  num_files = 0;

  while((dir = readdir(dirp)) != NULL)
  {
    if(dir->d_type == DT_REG && strncmp(dir->d_name, POST_FILE_PREFIX, prefix_len) == 0)
    {
      struct stat st;
      ssize_t path_len;

      if(num_files == 0)
      {
        capacity = 10;
        files = malloc(capacity * sizeof(PostFileStat));
        if(!files)
        {
          CosaPhpExtLog("failed to allocate files list\n");
          break;
        }
      }
      else if(num_files == capacity)
      {
        capacity *= 2;
        PostFileStat* rfiles = realloc(files, capacity * sizeof(PostFileStat));
        if(rfiles)
        {
          files = rfiles;
        }
        else
        {
          CosaPhpExtLog("failed to rellocate files list\n");
          free(files);
          files = 0;
          num_files = 0;
          break;
        }
      }

      path_len = snprintf(NULL, 0, "%s/%s", POST_DATA_DIR, dir->d_name);
      files[num_files].path = malloc(path_len + 1);
      if(!files[num_files].path)
      {
        CosaPhpExtLog("failed to allocate file path\n");
        post_files_stat_free(files, num_files);
        files = NULL;
        num_files = 0;
        break;
      }
      snprintf(files[num_files].path, path_len + 1, "%s/%s", POST_DATA_DIR, dir->d_name);

      stat(files[num_files].path, &st);
      
      files[num_files].age = now - TVSPEC_TO_SECONDS(st.st_atim);
      files[num_files].size = st.st_size;

      num_files++;
    }
  }

  closedir(dirp);
  
  *pfiles = files;
  return num_files;
}

void post_files_stat_free(PostFileStat* files, int num_files)
{
  int i;
  for(i = 0; i < num_files; ++i)
    if(files[i].path)
      free(files[i].path);
  free(files);
}

static int post_files_age_compare(const void * p1, const void * p2)
{
  const PostFileStat* s1 = (const PostFileStat*)p1;
  const PostFileStat* s2 = (const PostFileStat*)p2;
  if(s1->age > s2->age)
    return -1;
  else if(s1->age < s2->age)
    return 1;
  else
    return 0;
}

static int post_files_clean_directory(MPFDPart* part)
{
  PostFileStat* files;
  int num_files;
  int num_removed;
  int i;
  unsigned long long total_size;

  num_files = post_files_stat(&files);
  if(num_files <= 0)
    return 0;

  /*add total size of all files on disk plus the new file*/
  total_size = part->body_len;
  for(i = 0; i < num_files; ++i)
  {
    total_size += files[i].size;
  }

  /*sort files oldest to newest */
  qsort(files, num_files, sizeof(PostFileStat), post_files_age_compare);

  /*remove files in oldest to newest order until we have enough space*/
  num_removed = 0;
  while(total_size > POST_MAX_DISK_SPACE && num_removed < num_files)
  {
    CosaPhpExtLog("cleanup removing post file %s\n", files[num_removed].path);
    if(unlink(files[num_removed].path) < 0)
    {
      CosaPhpExtLog("cleanup failed to remove post file %s: %s", files[i].path, strerror(errno));
    }
    else
    {
      total_size -= files[num_removed].size;
    }
    num_removed++;
  }
  
  post_files_stat_free(files, num_files);

  return 0;
}
#else
static int post_files_clean_directory(MPFDPart* part)
{
  //simple remove all existing file thus only allowing a single file to be saved
  //sufficient for web ui since only webui-bwg has file upload and its for a single restore config
  CosaPhpExtLog("removing previous post uploads: " 
         "rm  -f " POST_DATA_DIR "/" POST_FILE_PREFIX "*" "\n");
  system("rm  -f " POST_DATA_DIR "/" POST_FILE_PREFIX "*");
}
#endif

static int write_upload_file(MPFDPart* part)
{
  if(part->body_len <= POST_MAX_FILESIZE)
  {
    char file_path[] = POST_FILE_TEMPLATE;
    size_t file_path_len;
    int file_fd;

    post_files_clean_directory(part);

    file_fd = mkstemp(file_path);
    if(file_fd > -1)
    {
      ssize_t write_len;

      write_len = write(file_fd, part->body, part->body_len);

      if(write_len == (ssize_t)part->body_len)
      {
        CosaPhpExtLog("file %s uploaded\n", file_path);
        part->tmp_file_name = strdup(file_path);
        part->file_error = UploadeErrOK;
      }
      else
      {
        CosaPhpExtLog("failed to write upload file %s, rc=%d, error:%s\n", file_path, (int)write_len, strerror(errno));
        part->file_error = UploadeErrFailedWrite;
      }
      close(file_fd);
    }
    else
    {
      CosaPhpExtLog("failed to open upload tmp file %s, error:%s\n", file_path, strerror(errno));
      part->file_error = UploadeErrFailedWrite;
    }
  }
  else
  {
    CosaPhpExtLog("failed to save upload file, file size %d exceeds limit %d\n", part->body_len, POST_MAX_FILESIZE);
    part->file_error = UploadeErrFailedWrite;
    return -1;
  }
  return part->file_error;
}

/* Examples:
  Content-Disposition: form-data; name="file"; filename="mrollinssavedconfig.CF2"
  Content-Disposition: form-data; name="VerifyPassword"
*/
static int parse_mpfd_content_disposition(char* line, char* eol, MPFDPart* part)
{
  char* data;
  char* name;
  char* value;

  part->name = NULL;
  part->file_name = NULL;

  data = strstr(line, "form-data");
  if(data)
  {
    data = strchr(data, ';');
    if(data)
      data++;
  }
  if(!data || data >= eol)
  {
    CosaPhpExtLog("%s error 1\n", __FUNCTION__);
    return -1;
  }
  while( parse_name_value_pair(&data, eol, &name, &value) == 0)
  {
    if(strcmp(name, "name") == 0)
      part->name = value;
    else if(strcmp(name, "filename") == 0)
      part->file_name = value;
    else
    {
      //TODO log unknown name
    }
  }
  if(part->name)
    return 0;
  else
    return -1;
}

/* Examples:
  Content-Type: application/octet-stream
*/
static int parse_mpfd_content_type(char* type, char* eol, MPFDPart* part)
{
  char* p;
  char* value = NULL;
  part->type = MPFDContentTypeNull;
  p = strchr(type, ':');
  if(!p || p>=eol)
  {
    /*TODO log error*/
    CosaPhpExtLog("%s error 1\n", __FUNCTION__);
    return -1;
  }
  p++;
  while(*p && p < eol && isspace(*p))
    p++;
  if(p>=eol)
  {
    /*TODO log error*/
    CosaPhpExtLog("%s error 2\n", __FUNCTION__);
    return -1;
  }
  value = p;
  while(p < eol && isalpha(*p))
    p++;
  part->stype = value;
  if(strncmp(value, "text/plain", 10) == 0)
  {
    part->type = MPFDContentTypeTextPlain;
  }
  else if(strncmp(value, "application/octet-stream", 24) == 0)
    part->type = MPFDContentTypeOctetStream;
  else
  {
    CosaPhpExtLog("%s error 3\n", __FUNCTION__);
  }
  if(part->type == MPFDContentTypeNull)
    return -1;
  else
    return 0;
}

static char* next_mpfd_line(char* cursor, char* eof, int* line_len)
{
  char* line = NULL;

  /*move to start of next line*/
  while(cursor < eof)
  {
    if(memcmp(cursor, "\r\n", 2) == 0 ||
       memcmp(cursor, "\0\n", 2) == 0)
    {
      cursor += 2;
      break;
    }
    cursor++;
  }

  if(cursor >= eof)
    return NULL;

  line = cursor;

  /*move to end of the line*/
  while(cursor < eof)
  {
    if(memcmp(cursor, "\r\n", 2) == 0)
    {
      break;
    }
    cursor++;
  }

  if(cursor >= eof)
    return NULL;

  *line_len = cursor - line;
  line[*line_len] = '\0';
  return line;
}

static int parse_mpfd_part(char** cursor, char* eof, const char* boundary, int boundary_len, MPFDPart** parts, int* parts_len)
{
  char* line;
  int line_len;

  MPFDPart part;
  init_mpfd_part(&part);
  while((line = next_mpfd_line(*cursor, eof, &line_len)))
  {
    if(line_len == 0)
      break;
    if(strncmp(line, "Content-Disposition", 19) == 0)
    {
      parse_mpfd_content_disposition(line, line + line_len, &part);
    }
    else if(strncmp(line, "Content-Type", 12) == 0)
    {
      parse_mpfd_content_type(line, line + line_len, &part);
    }
    *cursor = line + line_len;
  }

  if(!part.name)
  {
    //TODO log error
    return -1;
  }

  part.body = *cursor = line + 2;

  if(!part.body)
  {
    //TODO log_error;
    return -1;
  }

  /*search for start of next boundary*/
  while(*cursor < eof - boundary_len)
  {
    if(memcmp(*cursor, boundary, boundary_len) == 0)
    {
      part.body_len = (*cursor - 2) - part.body;
      *(part.body + part.body_len) = 0;
      break;
    }
    (*cursor)++;
  }

  if(*parts == NULL)
  {
    *parts_len = 1;
    *parts = malloc(sizeof(MPFDPart));
    if(!*parts)
    {
      CosaPhpExtLog("failed allocate mpfd parts\n");
      return -1;
    }
    memcpy(*parts, &part, sizeof(MPFDPart));
  }
  else
  {
    int len = *parts_len;
    len++;
    MPFDPart* rparts = realloc(*parts, sizeof(MPFDPart) * len);
    if(rparts)
    {
      *parts = rparts;
    }
    else
    {
      CosaPhpExtLog("failed reallocate mpfd parts\n");
      return -1;
    }

    memcpy(*parts+len-1, &part, sizeof(MPFDPart));
    (*parts_len)++;
  }
  return 0;
}

static void parse_mpfd(char* content_data, int content_len, const char* boundary, int boundary_len, MPFDPart** parts, int* parts_len)
{
  char* cursor = content_data;
  char* eof = content_data + content_len;

  while(cursor < eof)
  {
    while(cursor < eof - boundary_len)
    {
      if(memcmp(cursor, boundary, boundary_len) == 0)
        break;
      cursor++;
    }

    cursor += boundary_len;

    if(cursor < eof)
    {
      if(cursor <= eof - 2 && strncmp(cursor, "--", 2) == 0)
        break;

      parse_mpfd_part(&cursor, eof, boundary, boundary_len, parts, parts_len);
    }
  }
}

static void process_multipart_form_data(char* content_data, int content_len, char* boundary, int boundary_len)
{
  MPFDPart* parts = NULL;
  int parts_len = 0;
  int i;
  int num_files = 0;
  int num_posts = 0;
  int files_data_len = 0;
  int post_data_len = 0;
  char* cursor;

  parse_mpfd(content_data, content_len, boundary, boundary_len, &parts, &parts_len);
  CosaPhpExtLog("Got %d parts\n", parts_len);

  if(parts_len == 0)
    return;

  for(i=0; i<parts_len; ++i)
  {
    CosaPhpExtLog("PART\n\tname:%s\n\tfilename:%s\n\ttype=%d\n\tbody=%s\n\tbody_len=%d\n",
      parts[i].name, parts[i].file_name, parts[i].type, parts[i].body, parts[i].body_len);
  }

  /*write the files to tmp folder*/
  for(i=0; i<parts_len; ++i)
  {
    if(parts[i].file_name)
    {
      write_upload_file(&parts[i]);
    }
  }

  /*create _FILES data*/
  /*example:
    success (file was successfully saved to tmp folder):
      name=foo&type=application/octet-stream&size=231424&tmp_name=/tmp/jst_post_0000010123002133_file_mrollinssavedconfig.CF2&error=0
    error (failed to save file to tmp folder):
      name=foo&type=application/octet-stream&size=231424&tmp_name=&error=1
    if multiple, use ; for separator
  */
  for(i=0; i<parts_len; ++i)
  {
    if(parts[i].file_name)
    {
        if(num_files++)
          files_data_len++;/*for ; separator*/

        files_data_len += snprintf(NULL, 0, "id=%s&name=%s&type=%s&size=%d&tmp_name=%s&error=%d",
                            parts[i].name,
                            parts[i].file_name,
                            parts[i].stype ? parts[i].stype : "text/plain",
                            parts[i].body_len,
                            parts[i].tmp_file_name ? parts[i].tmp_file_name : "",
                            parts[i].file_error);
    }
  }

  if(files_data_len)
  {
    files_data_len++;/*null term*/
    files_data = cursor = malloc(files_data_len);
    if(!files_data)
    {
      CosaPhpExtLog("failed to allocate files data\n");
      return;
    }
    num_files = 0;
    for(i=0; i<parts_len; ++i)
    {
      if(parts[i].file_name)
      {
          if(num_files++)
            *cursor++ = ';';

          cursor += sprintf(cursor, "id=%s&name=%s&type=%s&size=%d&tmp_name=%s&error=%d",
                              parts[i].name,
                              parts[i].file_name,
                              parts[i].stype ? parts[i].stype : "text/plain",
                              parts[i].body_len,
                              parts[i].tmp_file_name ? parts[i].tmp_file_name : "",
                              parts[i].file_error);
      }
    }

    *cursor = 0;
    CosaPhpExtLog("files_data_len=%d\n", files_data_len);
    CosaPhpExtLog("WROTE %d\n", (int)(cursor - files_data));
    CosaPhpExtLog("_FILES=%s\n", files_data);
  }

  /*write non-file data for _POST*/
  for(i=0; i<parts_len; ++i)
  {
    if(!parts[i].file_name)
    {
      if(num_posts++)
        post_data_len++;/*for & separator*/

      post_data_len += snprintf(NULL, 0, "%s=%s", parts[i].name, parts[i].body);
    }
  }

  if(post_data_len > 0)
  {
    post_data_len++;/*null term*/
    post_data = cursor = malloc(post_data_len);
    if(!post_data)
    {
      CosaPhpExtLog("failed to allocate post data\n");
      return;
    }    
    num_posts = 0;
    for(i=0; i<parts_len; ++i)
    {
      if(!parts[i].file_name)
      {
        if(num_posts++)
          *cursor++ = '&';

        cursor += sprintf(cursor, "%s=%s", parts[i].name,  parts[i].body);
      }
    }
    *cursor = 0;
    CosaPhpExtLog("post_data_len=%d\n", post_data_len);
    CosaPhpExtLog("WROTE %d\n", (int)(cursor - post_data));
    CosaPhpExtLog("_POST=%s\n", post_data);
  }
  else
  {
    post_data = content_data;
  }

  for(i=0; i<parts_len; ++i)
  {
    if(parts[i].tmp_file_name)
      free(parts[i].tmp_file_name);
  }
  if(parts)
    free(parts);
}

#if DEBUG_POST_LOAD
int load_debug_post_data(char* content_data, int content_len)
{
  const char* path;
  FILE* pfile;
  int read_len;
  path = getenv("JST_DBG_POST_FILE");
  if(!path)
    return;
  pfile = fopen(path, "r");
  if(pfile)
  {
    CosaPhpExtLog("%s loading %s\n", __FUNCTION__, path);
    read_len = fread(content_data, 1, content_len, pfile);
    fclose(pfile);
    return read_len;
  }
  else
  {
    CosaPhpExtLog("%s failed to load %s\n", __FUNCTION__, path);
    return 0;
  }
}
#endif

#if DEBUG_POST_SAVE
void save_debug_post_data(const char* content_data, int content_len)
{
  char path[256];
  FILE* pfile;
  if(!jst_debug_file_name)
    return;
  snprintf(path, 255, "/tmp/jst_dbg_postFile%s", jst_debug_file_name);
  pfile = fopen(path, "w");
  if(pfile)
  {
    CosaPhpExtLog("%s saving %s\n", __FUNCTION__, path);
    fwrite(content_data, 1, content_len, pfile);
    fclose(pfile);
  }
  else
  {
    CosaPhpExtLog("%s failed to save %s\n", __FUNCTION__, path);
  }
}      
#endif

duk_ret_t ccsp_post_module_open(duk_context *ctx)
{
  const char* env_content_len= 0;
  char* content_data = 0;
  int content_len= 0;
  int read_len= 0;
  char* boundary = NULL;
  int boundary_len = 0;
  int content_type = 0;

  duk_push_object(ctx);
  duk_put_function_list(ctx, -1, ccsp_post_funcs);

  env_content_len = getenv("CONTENT_LENGTH");
  if(env_content_len)
  {
    content_len = atoi(env_content_len);

    if(content_len > POST_MAX_SIZE)
    {
      CosaPhpExtLog("post size %d exceeds limit %d\n", content_len, POST_MAX_SIZE);
      return 1;
    }
  
    if(content_len > 0)
    {
      content_data = (char*)malloc(content_len + 1);
      if(!content_data)
      {
        CosaPhpExtLog("failed to allocate content data\n");
        return 1;
      }
#if DEBUG_POST_LOAD
      if(jst_debug_file_name && access("/tmp/jst_enable_dbg_load", F_OK) == 0)
        read_len = load_debug_post_data(content_data, content_len);
      else
#endif
      read_len = fread(content_data, 1, content_len, stdin);
      content_data[content_len] = 0;
      if(read_len != content_len)
      {
        CosaPhpExtLog("failed to read post data\n");
      }
#if DEBUG_POST_SAVE
      if(jst_debug_file_name && access("/tmp/jst_enable_dbg_save", F_OK) == 0)
        save_debug_post_data(content_data, content_len);
#endif
      content_type = parse_content_type_header(&boundary, &boundary_len);
      if(content_type == HeaderContentTypeMPFD)
      {
        if(boundary)
        {
          process_multipart_form_data(content_data, content_len, boundary, boundary_len);
          free(boundary);
          free(content_data);
        }
        else
        {
          CosaPhpExtLog("failed parse mpfd boundary\n");
        }
      }
      else
      {
        if(content_type == HeaderContentTypeNull)
        {
          CosaPhpExtLog("failed parse content type header\n");
        }
        post_data = content_data;
      }
    }
  }

  return 1;
}


