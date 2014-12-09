/* Test using libmicrohttpd instead of custom code. */

//  clang microserver.c -o microserver -lmicrohttpd
 
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <microhttpd.h>
#include <string.h>
#include <stdio.h>

#define PORT 1313

#define N_THREADS 8

int answer (void *cls, struct MHD_Connection *connection, 
            const char *url, 
            const char *method, const char *version, 
            const char *upload_data, 
            size_t *upload_data_size, void **con_cls) {

    const char *page  = "<html><body>Hello, browser!</body></html>";
    struct MHD_Response *response;
    int ret;

    /* Look up query parameters, filling in defaults. */
    const char *lat_s = MHD_lookup_connection_value (connection, MHD_GET_ARGUMENT_KIND, "lat");
    double lat = (lat_s == NULL) ? 0.0 : atof(lat_s);
    const char *lon_s = MHD_lookup_connection_value (connection, MHD_GET_ARGUMENT_KIND, "lon");
    double lon = (lon_s == NULL) ? 0.0 : atof(lon_s);
    printf ("lat=%f lon=%f\n", lat, lon);
       
    response = MHD_create_response_from_buffer (strlen (page), (void*) page, MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response (connection, MHD_HTTP_OK, response);
    MHD_destroy_response (response);
    return ret;
}


int main () {

  struct MHD_Daemon *daemon;
  daemon = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL, &answer, NULL, 
                             MHD_OPTION_THREAD_POOL_SIZE, N_THREADS, MHD_OPTION_END);
                             
  if (NULL == daemon) return 1;
  getchar ();
  MHD_stop_daemon (daemon);
  return 0;
  
}

