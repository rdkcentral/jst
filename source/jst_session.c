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
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include "jst_internal.h"
#include <sys/sysinfo.h>
#include <stdint.h>
#include <utime.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/random.h>

#define RETURN_STRING(res) { duk_push_string(ctx, res); return 1; }
#define RETURN_TRUE { duk_push_true(ctx); return 1; }
#define RETURN_FALSE { duk_push_false(ctx); return 1; }

#define SESSION_PREFIX "jst_sess"
#define SESSION_PREFIX_LEN 8
#define SESSION_ID_BYTES_LENGTH 32 /*php default */
#define SESSION_ID_LENGTH (SESSION_PREFIX_LEN + SESSION_ID_BYTES_LENGTH)
#define SESSION_FILE_MAX_PATH 100
#define SESSION_TMP_DIR "/tmp"
#define SESSION_NUMBER_PRECISION 12
#define BYTE_TO_PRINTABLE_HEX_CODE(B) ( PRINTABLE_HEX_CODES[ (uint32_t)(B) % (uint32_t)(sizeof(PRINTABLE_HEX_CODES)-1) ] )

/*
  session data is stored to file in /tmp directory

  filename format: sess[0..pid][0..ms] or as printf sess%010d%06ld

  content format: key|type|value;[...]
  valid types: s,n,b  (string, number, boolean)

  example:
    cat sess0000024118523452
    fruit|s|apple;type|s|granny smith;quantity|n|12;organic|b|1;description|s|grown locally without harmful chemicals;price|n|3.95;
  
  We only keep the session_identifier pointer holding the session id.
  Any session data will be loaded into a global variable named $_SESSION.
  The javascript will call start to begin a session.
  The javascript will call getData to read any session data from disk into $_SESSION.
  The javascript will call setData any time any value on $_SESSION changed and the whole object will be saved to disk.
  The javascript can get the session id with getId, can determine if the session was started with getStatus, 
    and can end the session with destroy.
*/

static char* session_identifier = NULL;

static duk_ret_t session_start(duk_context *ctx)
{
  CosaPhpExtLog("%s: entered\n", __PRETTY_FUNCTION__);
  const char* cookie;
  /* if session already created then do nothing */
  if(session_identifier)
  {
    char path[SESSION_FILE_MAX_PATH];
    snprintf(path, SESSION_FILE_MAX_PATH, "%s/%s", SESSION_TMP_DIR, session_identifier);
    if(utime(path, NULL) != 0)
    {
      CosaPhpExtLog("failed to update last accesstime on file %s: %s", path, strerror(errno));
      RETURN_FALSE;
    }
    RETURN_TRUE;
  }

  session_identifier = (char*)malloc(SESSION_ID_LENGTH+1);
  if(!session_identifier)
  {
    CosaPhpExtLog("Failed to allocate session_identifier!\n");
    RETURN_FALSE;
  }
  memset(session_identifier, 0, SESSION_ID_LENGTH+1);

  cookie = getenv("HTTP_COOKIE");
  if(cookie)
  {
    CosaPhpExtLog("%s: cookie %s\n", __PRETTY_FUNCTION__, cookie);
    /*load session id from cookie*/
    const char* sesid = NULL;
    const char* tmp = cookie;
    while (tmp = strstr(tmp, "DUKSID="))
    {
      sesid= tmp;
      tmp++;
    }
    CosaPhpExtLog("%s: sesid %s\n", __PRETTY_FUNCTION__, sesid);
    if(sesid)
    {
      sesid += 7;
      int len = strlen(sesid);
      if(len >= SESSION_ID_LENGTH)
      {
           int idx = SESSION_PREFIX_LEN;
           int isvalid = 1;
           /* Validate session ID*/
           while ( idx < SESSION_ID_LENGTH) {
              if (!isalnum(sesid[idx])) {
                      CosaPhpExtLog("Invalid SessionID\n");
                      isvalid = 0;
                      break;
              }
              idx++;
           }
           if(isvalid)
           {
             sesid = strtok(sesid, ";");
             const char filename[SESSION_FILE_MAX_PATH];
             snprintf(filename, SESSION_FILE_MAX_PATH, "%s/%s", SESSION_TMP_DIR, sesid);
             CosaPhpExtLog("%s: Checking for Session file %s\n", __PRETTY_FUNCTION__, filename);
             if (access(filename, F_OK) == 0)
             {
               CosaPhpExtLog("%s: Session file %s exists\n", __PRETTY_FUNCTION__, filename);
               strncpy(session_identifier, sesid, SESSION_ID_LENGTH);
             } else {
               CosaPhpExtLog("%s: Failed to read Session file %s\n", __PRETTY_FUNCTION__, filename);
             }
           }
      } else {
           CosaPhpExtLog("Invalid SessionID Entropy\n");
      }
    }
  }
  if(!session_identifier[0])
  {
   CosaPhpExtLog("Invalid Session\n");
   RETURN_FALSE;
  }

  RETURN_TRUE;
  return 1;
}
static duk_ret_t session_create(duk_context *ctx)
{
  CosaPhpExtLog("%s: entered\n", __PRETTY_FUNCTION__);
  /*create a new one*/
  static const char PRINTABLE_HEX_CODES[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  int i = 0, n = 0;
  uint8_t bytes[SESSION_ID_BYTES_LENGTH];
  char* session_id = NULL;

  session_id = (char*)malloc(SESSION_ID_BYTES_LENGTH+1);
  n = syscall(SYS_getrandom, bytes, SESSION_ID_BYTES_LENGTH, 0);
  if(n != SESSION_ID_BYTES_LENGTH)
  {
    CosaPhpExtLog("failed to get random bytes\n");
    free(session_id);
    RETURN_FALSE;
  }

  for(i = 0; i < SESSION_ID_BYTES_LENGTH; ++i)
  {
    session_id[i] = BYTE_TO_PRINTABLE_HEX_CODE(bytes[i]);
  }

  session_identifier = (char*)malloc(SESSION_ID_LENGTH+1);
  if(!session_identifier)
  {
    CosaPhpExtLog("Failed to allocate session_identifier!\n");
    RETURN_FALSE;
  }
  memset(session_identifier, 0, SESSION_ID_LENGTH+1);

  session_id[SESSION_ID_BYTES_LENGTH] = '\0';
  snprintf(session_identifier, SESSION_ID_LENGTH+1, "%s%s", SESSION_PREFIX, session_id);

  RETURN_TRUE;
  return 1;
}

static duk_ret_t session_get_data(duk_context *ctx)
{
  char* contents;
  size_t content_len;
  char filename[SESSION_FILE_MAX_PATH+1];
  duk_idx_t idx;
  size_t i;
  size_t j;
  char* key;
  char* type;
  char* value;
  int valid;
  int count;
  int state;/*0=key, 1=type, 2=value*/
  char* s1;
 
  if(session_identifier == NULL)
  {
    RETURN_FALSE;
  }

  valid = 0; /*valid becomes 1 only if we process a valid data file completely*/

  idx = duk_push_object(ctx);

  snprintf(filename, SESSION_FILE_MAX_PATH, "%s/%s", SESSION_TMP_DIR, session_identifier);

  CosaPhpExtLog( "session_get_data filename=%s\n", filename );

  if(read_file(filename, &contents, &content_len) > 0)
  {
    s1 = contents;
    key = type = value = NULL;
    count = state = 0;

    for(i=0; i<content_len; ++i)
    {
      if(state == 0)
      {
        if(!key)
          key = s1+i;
        if(s1[i] == '|')
        {
          s1[i] = 0;
          state++;
        }
      }
      else if(state == 1)
      {
        if(!type)
          type = s1+i;
        if(s1[i] == '|')
        {
          s1[i] = 0;
          state++;
        }
      }
      else if(state == 2)
      {
        if(!value)
          value = s1+i;
        if(s1[i] == ';')
        {
          s1[i] = 0;
          
          /*process the record*/
          if(strlen(type) != 1 || (*type != 's' && *type != 'n' && *type != 'b'))
          {
            fprintf(stderr, "%s: found invalid type in file %s", __PRETTY_FUNCTION__, filename);
            break;
          }

          if(*type == 's')
          {
            duk_push_string(ctx, value);
          }
          else if(*type == 'n')
          {
            duk_push_int(ctx, atof(value));
          }
          else
          {
            duk_push_boolean(ctx, atoi(value));
          }

          duk_put_prop_string(ctx, idx, key);

          /*reset for next record*/
          count++;
          key = type = value = NULL;
          state = 0;

          /*check if we are at end of file content, ignoring whitespace*/
          for(j = i+1; j < content_len; ++j)
          {
            if(!isspace(s1[j]))
            {
              /*more content found*/
              break;
            }
          }
          if(j == content_len)
          {
            /*all content was processed succesfully*/
            valid = 1;
            break;
          }
        }
      }
    }
    free(contents);
    CosaPhpExtLog( "session_get_data succeeded to read filename=%s\n", filename );
  }
  else
  {
    CosaPhpExtLog( "session_get_data failed to read filename=%s\n", filename );
    fprintf(stderr, "%s: failed to read file %s\n", __PRETTY_FUNCTION__, filename);
  }

  if(!valid)
  {
    /*remove the invalid array and create an empty one*/
    duk_pop(ctx);
    duk_push_object(ctx);
  }

  return 1;
}

static duk_ret_t session_set_data(duk_context *ctx)
{
  FILE* pfile;
  char filename[SESSION_FILE_MAX_PATH];
  
  if(session_identifier == NULL)
  {
    fprintf(stderr, "%s: session not started", __PRETTY_FUNCTION__);
    RETURN_FALSE;
  }

  if(!duk_is_object(ctx,0))
  {
    fprintf(stderr, "%s: parameter is not an object", __PRETTY_FUNCTION__);
    RETURN_FALSE;
  }

  snprintf(filename, SESSION_FILE_MAX_PATH, "%s/%s", SESSION_TMP_DIR, session_identifier);

  CosaPhpExtLog( "session_set_data filename=%s\n", filename );

  pfile = fopen(filename, "w");

  if(!pfile)
  {
    CosaPhpExtLog( "session_set_data failed to open filename=%s\n", filename );
    fprintf(stderr, "%s: failed to open file %s", __PRETTY_FUNCTION__, filename);
    RETURN_FALSE;
  }

  duk_enum(ctx, 0, 0);

  while (duk_next(ctx, -1, 1)) 
  {
    duk_int_t type;

    type = duk_get_type(ctx, -1);

    if(type == DUK_TYPE_STRING)
    {
      fprintf(pfile, "%s|%c|%s;", duk_get_string(ctx, -2), 's', duk_get_string(ctx, -1));
    }
    else if(type == DUK_TYPE_NUMBER)
    {
      fprintf(pfile, "%s|%c|%.*f;", duk_get_string(ctx, -2), 'n', SESSION_NUMBER_PRECISION, (double)duk_get_number(ctx, -1));
      
    }
    else if(type == DUK_TYPE_BOOLEAN)
    {
      fprintf(pfile, "%s|%c|%d;", duk_get_string(ctx, -2), 'b', (int)duk_get_boolean(ctx, -1));
    }
    else
    {
      fprintf(stderr, "%s: unsupported type %d", __PRETTY_FUNCTION__, (int)type);
      printf("%s: unsupported type %d", __PRETTY_FUNCTION__, (int)type);
    }

    duk_pop(ctx);/*pop key*/
    duk_pop(ctx);/*pop val*/
  }

  duk_pop(ctx);

  fclose(pfile);

  CosaPhpExtLog( "session_set_data file written %s\n", filename );

  RETURN_TRUE;
}

static duk_ret_t session_get_id(duk_context *ctx)
{
  if(session_identifier)
  {
    RETURN_STRING(session_identifier);
  }
  else
  {
    RETURN_FALSE;
  }
}

static duk_ret_t session_get_status(duk_context *ctx)
{
  if(session_identifier)
  {
    RETURN_TRUE;
  }
  else
  {
    RETURN_FALSE;
  }
}

static duk_ret_t session_destroy(duk_context *ctx)
{
  char filename[SESSION_FILE_MAX_PATH];

  if(session_identifier)
  {
    /*remove the session file*/
    snprintf(filename, SESSION_FILE_MAX_PATH, "%s/%s", SESSION_TMP_DIR, session_identifier);

    CosaPhpExtLog( "session_destroy removing %s\n", filename );

    unlink(filename);

    free(session_identifier);
    session_identifier = NULL;
    RETURN_TRUE;
  }
  else
  {
    RETURN_FALSE;
  }
}

static const duk_function_list_entry ccsp_session_funcs[] = {
  { "start", session_start, 0 },
  { "create", session_create, 0 },
  { "getId", session_get_id, 0 },
  { "getData", session_get_data, 0 },
  { "setData", session_set_data, 1 },
  { "getStatus", session_get_status, 0 },
  { "destroy", session_destroy, 0 },
  { NULL, NULL, 0 }
};

duk_ret_t ccsp_session_module_open(duk_context *ctx)
{
  duk_push_object(ctx);
  duk_put_function_list(ctx, -1, ccsp_session_funcs);
  return 1;
}
