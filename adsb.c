 /*
 * Needs libcjson-dev, libcurl
 *
 * build:- cc -oadsb adsb.c -lm -lcjson -lcurl
 *
 *
 * need to add some getopt stuff to change things at runtime
 * need to make the socket reads non-blocking
 *
 *
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <math.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define ADSB_PORT    30003
#define ADSB_SRV     "192.168.1.30"

#define HERE_LAT     54.098218
#define HERE_LONG    -4.673231

#define PI           3.14159

#define EARTH_RADIUS 3959  // Miles

#define TICK_TIME    1  // second
#define TIMEOUT      20 // seconds

#define TABLE_SIZE   250
#define MAX_DISTANCE 15

#define MSG_SIZE     1025

#define FLIGHT_ID_SIZE 10

int ticks;
int msgCount;

double here_lat;
double here_long;
int tick_time;

typedef struct
{
    char *response;
    size_t size;
} CURLMEM;

typedef struct
{
    unsigned long int hexId;
    double distance;
    unsigned long int height;
} DISTANCE;

typedef struct trackedAircraft
{
    unsigned long int hexId;
    double distance;
    char flightId[10];
} TRACKED;

static size_t curlCallback(char *chunkPtr, size_t size,
                                    size_t byteCount, void *cbDataPtr)
{
    CURLMEM *respPtr;
    char *respDataPtr;
    size_t newSize;
    size_t rtn;

    rtn = 0;

    respPtr = (CURLMEM *)cbDataPtr;
    newSize = respPtr -> size + byteCount + 1;
    respDataPtr = realloc(respPtr -> response, newSize);
    if(respDataPtr == 0)
    {
        printf("realloc() error\n");
    }
    else
    {
        respPtr -> response = respDataPtr;
        memcpy(&(respPtr -> response[respPtr -> size]),
                                                   chunkPtr, byteCount);
        respPtr -> size = (respPtr -> size) + byteCount;
        respPtr -> response[respPtr -> size] = '\0';

        rtn = byteCount;
    }

    return rtn;
}

void showItem(char *name, cJSON *item, char endChr)
{
    if(cJSON_IsString(item))
    {
        if(item -> valuestring != NULL)
        {
            if(*name != '\0')
            {
                printf("%-13s: ", name);
            }
            else
            {
                printf(", ");
            }
            printf("%s%c", item -> valuestring, endChr);
        }
    }
    else
    {
        // printf("Not a string\n");
    }
}


void showJsonInfo(char *jsonInfo)
{
    cJSON *root;
    char *dPtr;
    cJSON *item;

    if(jsonInfo == NULL)
    {
        printf("Null pointer to json info\n");
    }
    else
    {
        root = cJSON_Parse(jsonInfo);
        if(root == NULL)
        {
            printf("JSON parse error\n");
            while(*jsonInfo != '\0')
            {
                putchar(*jsonInfo);
                jsonInfo++;
            }
            printf("\n");
        }
        else
        {
            item = cJSON_GetObjectItemCaseSensitive(root, "RegisteredOwners");
            showItem("Owner", item, '\n');
            item = cJSON_GetObjectItemCaseSensitive(root, "Registration");
            showItem("Registration", item, '\n');
            item = cJSON_GetObjectItemCaseSensitive(root, "Manufacturer");
            showItem("Type", item, '\0');
            item = cJSON_GetObjectItemCaseSensitive(root, "Type");
            showItem("", item, '\n');

            cJSON_Delete(root);
        }
    }
}

void lookupHexId(unsigned long hexid, CURLMEM *curlResponse)
{
    char url[255];
    CURL *curl;
    CURLcode status;

    sprintf(url, "https://hexdb.io/api/v1/aircraft/%06lx", hexid);

    curl = curl_easy_init();
    if(curl)
    {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)curlResponse);
        curl_easy_setopt(curl, CURLOPT_URL, url);

        curlResponse -> size = 0;
        curlResponse -> response = NULL;

        status = curl_easy_perform(curl);
        
        if(status != CURLE_OK)
        {
            printf("curl_easy_perform() failed: %s\n",
                                                   curl_easy_strerror(status));
        }

        curl_easy_cleanup(curl);
    }
}

static void timeoutHandler(int signo)
{
    if(msgCount)
    {
        putchar('!');
    }
    else
    {
        putchar('X');
    }
    fflush(stdout);

    if(ticks)
    {
        ticks--;
    }

    alarm(tick_time);
}

char *findField(int f)
{
    char *tokPtr;

    tokPtr = NULL - 1;

    while(f && tokPtr != NULL)
    {
        tokPtr = strtok(NULL, ",");
        f--; 
    }

    return tokPtr;       
}

char *parseBuffer(char *buff, unsigned long *hexId, unsigned long int *height, double *lattitude, double *longitude)
{
    char *str;

    strtok(buff, ",");

    str = findField(4);
    if(str != NULL)
    {
        *hexId = strtol(str, NULL, 16);
        str = findField(6);
        if(str != NULL)
        {
            *height = strtoul(str, NULL, 10);
            str = findField(1);
            if(str != NULL)
            {
                *lattitude = strtod(str, NULL);
                str = findField(1);
                if(str != NULL)
                {
                    *longitude = strtod(str, NULL);
                }
            }
        }
    }

    return str;
}

int getFlightId(char *buffer, unsigned long int hexId, char *flightId)
{
    unsigned long int msgHexId;
    char *str;

    strtok(buffer, ",");

    str = findField(4);
    if(str != NULL)
    {
        msgHexId = strtol(str, NULL, 16);
        if(msgHexId == hexId)
        {
            str = findField(6);
            if(str != NULL)
            {
                strncpy(flightId, str, FLIGHT_ID_SIZE);
                str = flightId;
                while(*str != '\0')
                {
                    if(*str == ' ')
                    {
                        *str = '\0';
                    }
                    else
                    {
                        str++;
                    }
                }
            }
        }
    }
}

int readLine(int fd, char *buffer, int maxLen)
{
    char inChar;
    int readed;


    readed = 0;
    do
    {
        readed = readed + read(fd, &inChar, 1);
        if(inChar != '\n')
        {
            *buffer = inChar;
            buffer++;
            maxLen--;
        }
    }
    while(inChar != '\n' && maxLen > 0);

    *buffer = '\0';

    return readed;
}

void degToRadian(double *deg, double *rad)
{
    *rad = *deg * PI / 180;
}

void calcDistance(double *lattitude, double *longitude, double *distance)
{
    double dLat;
    double dLon;
    double latRad;
    double latHereRad;
    double a;
    double c;
    double x;

    degToRadian(lattitude, &latRad);
    degToRadian(&here_lat, &latHereRad);

    x = *lattitude - here_lat;
    degToRadian(&x, &dLat);

    x = *longitude - here_long;
    degToRadian(&x, &dLon);

    a = pow(sin(dLat / 2), 2) + pow(sin(dLon / 2), 2) * cos(latRad) * cos(latHereRad);
    c = 2 * asin(sqrt(a));

    *distance = EARTH_RADIUS * c;
}

DISTANCE *findNearest(int c, DISTANCE *aircraft)
{
    int x;
    DISTANCE *rtn;
    DISTANCE *xPtr;
    DISTANCE *yPtr;

    rtn = aircraft;
    for(x = 1; x < c; x++)
    {
       xPtr = &aircraft[x];
       yPtr = &aircraft[x - 1];
       if(xPtr -> distance < yPtr -> distance)
       {
           rtn = xPtr;
       } 
    }

    return rtn;
}

int connectTo(char *host, int port)
{
    struct sockaddr_in servAddr;
    int fd;
    int status;
    int rtn;

    rtn = -1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd > 0)
    {
        servAddr.sin_family = AF_INET;
        servAddr.sin_port = htons(port);

        if(inet_pton(AF_INET, host, &servAddr.sin_addr) > 0)
        {
            if(connect(fd, (struct sockaddr*)&servAddr, sizeof(servAddr)) == 0)
            {
                rtn = fd;
            }
        }
    }

    return rtn;
}

void showFlightSummary(DISTANCE *a, char *flightId)
{
    printf("Aircraft     : 0x%06x", a -> hexId);
    if(*flightId != '\0')
    {
        printf(" (%s)", flightId);
    }
    printf(", %0.2lf miles, %d feet\n", a -> distance, a -> height);
}

int main(int argc, char const* argv[])
{
    int fd;
    char *msg;
    DISTANCE *aircraft;
//    unsigned long int *hexId;
    char *flightId;
//    double *distance;
    unsigned long int lastLookup;
    double lattitude;
    double longitude;
    DISTANCE *nearest;
    int inRange;
    int c;
    int msg_3;
    CURLMEM jsonInfo;
    char adsb_srv[64];
    int adsb_port;
    int table_size;
    int timeout;
    double max_distance;

    strcpy(adsb_srv, ADSB_SRV);
    adsb_port = ADSB_PORT;
    table_size = TABLE_SIZE;
    tick_time = TICK_TIME;
    max_distance = MAX_DISTANCE;
    here_lat = HERE_LAT;
    here_long = HERE_LONG;
    timeout = TIMEOUT;

    printf("Server      : %s\n", adsb_srv);
    printf("Port number : %d\n", adsb_port);
    printf("Table size  : %d\n", table_size);
    printf("Tick time   : %d seconds\n", tick_time);
    printf("RX period   : %d ticks (= %d seconds)\n", timeout, timeout * tick_time);
    printf("Here        : %lf, %lf\n", here_lat, here_long);
    printf("Max distance: %0.1lf miles\n", max_distance);
    printf("\n");

    aircraft = (DISTANCE *)malloc(sizeof(DISTANCE) * table_size);
    if(aircraft == NULL)
    {
        printf("aircraft malloc() failed\n");
        return -1;
    }

    msg = malloc(MSG_SIZE);
    if(msg == NULL)
    {
        printf("msg buffer malloc() failed\n");
        return -1;
    }

    flightId = malloc(FLIGHT_ID_SIZE);

    printf("Connecting to %s:%d... ", adsb_srv, adsb_port);
    fflush(stdout);
    fd = connectTo(adsb_srv, adsb_port);
    if(fd == -1)
    {
        printf("Failed to connect\n");
        return -1;
    }

    printf("OK\n");

    curl_global_init(CURL_GLOBAL_ALL);

    signal(SIGALRM, timeoutHandler); 
    alarm(tick_time);

    *flightId = '\0';
    lastLookup = 0;
    while(1)
    {
        inRange = 0;
        c = 0;
        ticks = timeout;
        msgCount = 0;
        msg_3 = 0;
        printf("Receiving ");
        fflush(stdout);
        while(c < table_size && ticks)
        {
            readLine(fd, msg, MSG_SIZE);
            msgCount++;
            if(strncmp(msg, "MSG,3", 5) == 0)
            {
                msg_3++;
                if(parseBuffer(
                  msg, &aircraft[c].hexId, &aircraft[c].height, &lattitude, &longitude) != NULL)
                {
                    calcDistance(&lattitude, &longitude, &aircraft[c].distance);
                    if(aircraft[c].distance < max_distance)
                    {
                        c++;
                    }
                }
            }
        }

        printf(" %d/%d/%d\n", msg_3, c, msgCount);

        if(c)
        {
            nearest = findNearest(c, aircraft);
            if(nearest -> hexId == lastLookup)
            {
                showFlightSummary(nearest, flightId);
            }
            else
            {
                lastLookup = nearest -> hexId;
                flightId[0] = '\0';
                c = 0;
                printf("Identifying ");
                fflush(stdout);
                while(c < 100 && flightId[0] == 0)
                {
                    readLine(fd, msg, MSG_SIZE);
                    if(strncmp(msg, "MSG,1", 5) == 0)
                    {
                        c++;
                        getFlightId(msg, nearest -> hexId, flightId);
                    }
                }

                printf("\n\n");
                showFlightSummary(nearest, flightId);
                lookupHexId(nearest -> hexId, &jsonInfo);
                showJsonInfo(jsonInfo.response);
                free(jsonInfo.response);
            }
        }
        else
        {
            flightId[0] = '\0';
            printf("*** Nothing nearby ***\n");
        }
    }

    curl_global_cleanup();
    close(fd);

    return 0;
}
