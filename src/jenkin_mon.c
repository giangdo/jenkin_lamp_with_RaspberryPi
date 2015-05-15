#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "jenkin_mon.h"
#include <dirent.h> // For opendir() and closedir()
#include <sys/stat.h> // For stat() and mkdir()

//--------------------------------------------------------------------------------------------------
// README before read source code
//--------------------------------------------------------------------------------------------------
//* Version v1.0:
//   + Design:
//      - Curl Threads: Multiple threads that will execute curl command to get information about
//                      many jobs from jenkins server
//         . Each jobs information will be stored in 2 files statusInfoFile, lastBuildInfoFile.
//
//      - Color Thread: One thread to evaluate color for all group base on information read from
//                      statusInfoFile and lastBuildInfoFile files.
//         . Evaluated color will be store in global variable
//
//      - Led Thread: One thread to control led
//         . Read data from Evaluated color and control GPIO
//   + Bugs:
//      - Race condition between Curl Threads and Color Thread:
//         -> at the time Curl Thread write contain into file, Color Thread read file
//      - Race condition between Color Thread and Led Thread:
//         -> at the time Color Thread write into Evaluatied Color, Led Thread read this infomation
//
//* Version v1.2:
//   + Design:
//      - We have 2 threads to manage a group:
//         . Color Thread:
//            , execute curl command to get information of all jobs in group
//            , evaluate color and modify the evaluated color
//         . Led Thread:
//            , get evaluated color and control led
//
//* Version v2.0:
//    + Design:
//         - devide threads base on groups
//
//* Version v3.0:
//    + Feature: support command line to control/config program
//         - start program
//            $jenkinLight start
//         - stop program
//            $jenkinLight stop
//         - change config file
//            $jenkinLight add configfile.xml
//         - show current config file
//            $jenkinLight show config
//         - show current led status
//            $jenkinLight show led
//
//* Version v4.0:
//    + Feature: use opensaf to support Service Availability feature!
//
//* Version v5.0:
//    + Feature: design USB to GIPI module to control multiple jenkins led status

// Option to use authorized account to get info from jenkin server or not
#define USE_ANY_AUTHORIZED_IN_CURL 1

//----------------------------------------------------------------
// Global variable
//----------------------------------------------------------------
char* g_xmlFile = "jobsJenkinConfig.xml";

// Option to control verbose log
bool g_isVerbose = false;

// Option to control led
const u_int8 g_ledAnimeTime = 1; // in second
bool g_isCtrlRealLed = false;    // Defaut -> do not control real GPIO led

// Option to deamonize
bool g_isDaemon = false;

/* Termination flag */
static bool g_terminateAll = false;
static pthread_mutex_t g_terminateLock;

//----------------------------------------------------------------------------
// Handle for SIGINT and SIGTERM
//----------------------------------------------------------------------------
static void sig_term(int isig)
{
   pthread_mutex_lock(&g_terminateLock);
   g_terminateAll = true;
   pthread_mutex_unlock(&g_terminateLock);
}

//----------------------------------------------------------------------------
// Handle for SIGCHLD
//----------------------------------------------------------------------------
static void sig_chld(int sig)
{
	/*
	 * This function will never be executed, however it is
	 * needed to make sure SIGCHLD is not set to SIG_IGN,
	 * in which case we can't wait() on any child.
	 */
}

//----------------------------------------------------------------------------
// Exit program, try to terminate all threads before exit
//----------------------------------------------------------------------------
static void exitNow()
{
   pthread_mutex_lock(&g_terminateLock);
   g_terminateAll = true;
   pthread_mutex_unlock(&g_terminateLock);
}

//----------------------------------------------------------------------------
// Tail group: NULL or last have data inside but p_nextGroup = NULL
// Get tail group, tail group is the group in which memory was allocated but
// data inside is set to zero. So that p_nextGroup is NULL. Tail group can be
// understand as last empty data group. Head group can be tail group if there
// is no empty data inside it
//----------------------------------------------------------------------------
GroupInfoT* getTailGroup(GroupInfoT* p_headGroup)
{
   if (!p_headGroup)
   {
      return NULL;
   }
   GroupInfoT* p_curGroup = NULL;
   for (p_curGroup = p_headGroup;
        p_curGroup->p_nextGroup;
        p_curGroup = p_curGroup->p_nextGroup);
   return p_curGroup;
}

//----------------------------------------------------------------------------
// Print All Jobs's information
//----------------------------------------------------------------------------
void printAllJobInfo(JobInfoT* p_jobHead)
{
   JobInfoT* p_job = NULL;
   for (p_job = p_jobHead; p_job; p_job = p_job->p_nextJob)
   {
      printJobInfo(p_job);
   }
}

//----------------------------------------------------------------------------
// Print Job's information
//----------------------------------------------------------------------------
void printJobInfo(JobInfoT* p_job)
{
   if (p_job)
   {
      printf("job name: %s\n"\
             "job path: %s\n"\
             "status file: %s\n"\
             "last status file: %s\n",\
             p_job->jobName,
             p_job->jobPath,
             p_job->statusInfoFile,
             p_job->lastBuildInfoFile);
   }
}

//----------------------------------------------------------------------------
// Parsing Information of job (job attribute) in xml file to a JobInfoT data structure
//----------------------------------------------------------------------------
bool parseJobAttr(xmlDoc *doc, xmlNode *jobNode, JobInfoT* p_job)
{
   if (jobNode->children == NULL)
   {
      printf("Don't have any job attribute in this job in XML file\n");
      return false;
   }
   xmlNode* jobAttrNode = NULL;
   for (jobAttrNode = jobNode->children; jobAttrNode;
        jobAttrNode = jobAttrNode->next)
   {
      if (jobAttrNode->type == XML_ELEMENT_NODE)
      {
         if ((!strcmp(jobAttrNode->name, "jobpath")) ||
             (!strcmp(jobAttrNode->name, "jobname")))
         {
            xmlChar* key = xmlNodeListGetString(doc, jobAttrNode->xmlChildrenNode, 1);
            if (!strcmp(jobAttrNode->name, "jobpath"))
            {
               p_job->jobPath = malloc((strlen(key) + 1) * sizeof(char));
               strcpy(p_job->jobPath, key);
            }
            else if (!strcmp(jobAttrNode->name, "jobname"))
            {
               p_job->jobName = malloc((strlen(key) + 1) * sizeof(char));
               strcpy(p_job->jobName, key);
            }
            xmlFree(key);
         }
         else
         {
            printf("Wrong job attribute: %s\n", jobAttrNode->name);
            return false;
         }
      }
   }
   if (g_isVerbose)
   {
      printJobInfo(p_job);
   }
   return true;
}

//----------------------------------------------------------------------------
// Parsing Information of jobs in xml file to group data structure
//----------------------------------------------------------------------------
bool parseJobsInfo(xmlDoc *doc, xmlNode *jobsNode, JobInfoT** p_headJob)
{
   xmlNode* jobNode = NULL;
   if (!jobsNode->children)
   {
      printf("Don't have any job in group\n");
      return false;
   }
   JobInfoT* p_curJob = NULL;
   for (jobNode = jobsNode->children; jobNode; jobNode = jobNode->next)
   {
      if (jobNode->type == XML_ELEMENT_NODE)
      {
         if (!strcmp(jobNode->name, "job"))
         {
            JobInfoT jobInfoTemp;
            memset(&jobInfoTemp, 0, sizeof(JobInfoT));
            if (parseJobAttr(doc, jobNode, &jobInfoTemp))
            {
               if (!p_curJob)
               {
                  // First time -> head job = NULL
                  // Allocate memory for head job
                  p_curJob = malloc(sizeof(JobInfoT));
                  memset(p_curJob, 0, sizeof(JobInfoT));
                  *p_headJob = p_curJob;
               }
               else
               {
                  // Allocate memory for next job
                  p_curJob->p_nextJob = malloc(sizeof(JobInfoT));
                  p_curJob = p_curJob->p_nextJob;
               }

               // Get all job data to current job
               *p_curJob = jobInfoTemp;
               p_curJob->p_nextJob = NULL;
            }
            else
            {
               printf("Can not parse job attribute\n");
               return false;
            }

         }
         else
         {
            printf("Wrong ""job"" text: %s\n", jobNode->name);
            return false;
         }
      }
   }
   return true;
}

//----------------------------------------------------------------------------
// Parsing Information of Group in xml file to group data structure
//----------------------------------------------------------------------------
bool parseGroupAttr(xmlDoc *doc, xmlNode *groupNode, GroupInfoT* p_group)
{
   if (groupNode->children == NULL)
   {
      printf("Don't have any group attribute in this group in XML file\n");
      return false;
   }
   xmlNode* groupAttrNode = NULL;
   for (groupAttrNode = groupNode->children; groupAttrNode;
        groupAttrNode = groupAttrNode->next)
   {
      if (groupAttrNode->type == XML_ELEMENT_NODE)
      {
         if ((!strcmp(groupAttrNode->name, "groupname")) ||
             (!strcmp(groupAttrNode->name, "server")) ||
             (!strcmp(groupAttrNode->name, "username")) ||
             (!strcmp(groupAttrNode->name, "password")) ||
             (!strcmp(groupAttrNode->name, "red_led")) ||
             (!strcmp(groupAttrNode->name, "green_led")) ||
             (!strcmp(groupAttrNode->name, "blue_led")) ||
             (!strcmp(groupAttrNode->name, "display_timeout")) ||
             (!strcmp(groupAttrNode->name, "last_build_threshold")))
         {
            xmlChar* key = xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
            if (!strcmp(groupAttrNode->name, "groupname"))
            {
               p_group->groupName = malloc((strlen(key) + 1) * sizeof(char));
               strcpy(p_group->groupName, key);
            }
            else if (!strcmp(groupAttrNode->name, "server"))
            {
               p_group->server.serverName = malloc((strlen(key) + 1) * sizeof(char));
               strcpy(p_group->server.serverName, key);
            }
            else if (!strcmp(groupAttrNode->name, "username"))
            {
               p_group->server.userName = malloc((strlen(key) + 1) * sizeof(char));
               strcpy(p_group->server.userName, key);
            }
            else if (!strcmp(groupAttrNode->name, "password"))
            {
               p_group->server.passWord = malloc((strlen(key) + 1) * sizeof(char));
               strcpy(p_group->server.passWord, key);
            }
            else if (!strcmp(groupAttrNode->name, "red_led"))
            {
               p_group->gpio.redLed = atoi(key);

            }
            else if (!strcmp(groupAttrNode->name, "green_led"))
            {
               p_group->gpio.greLed = atoi(key);
            }
            else if (!strcmp(groupAttrNode->name, "blue_led"))
            {
               p_group->gpio.bluLed = atoi(key);
            }
            else if (!strcmp(groupAttrNode->name, "display_timeout"))
            {
               p_group->displaySuccessTimeout = atoi(key);
            }
            else if (!strcmp(groupAttrNode->name, "last_build_threshold"))
            {
               p_group->lastBuildThreshold= atoi(key);
            }
            xmlFree(key);
         }
         else if (!strcmp(groupAttrNode->name, "jobs"))
         {
            if(!parseJobsInfo(doc, groupAttrNode, &p_group->p_allJobs))
            {
               printf("Can not parse JobsInfo\n");
               return false;
            }
         }
         else
         {
            printf("Wrong group attribute: %s\n",groupAttrNode->name);
            return false;
         }
      }
   }
   if (g_isVerbose)
   {
      printGroupInfo(p_group);
   }
   return true;
}

//----------------------------------------------------------------------------
// Init led
//----------------------------------------------------------------------------
void initAllGroupLed(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      // TODO: need to init gpio in this function instead of using bashscript
   }
}

//----------------------------------------------------------------------------
// Printf all Groups database
//----------------------------------------------------------------------------
void printAllGroupInfo(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      printGroupInfo(p_group);
   }
}

//----------------------------------------------------------------------------
// Printf Group database
//----------------------------------------------------------------------------
void printGroupInfo(GroupInfoT* p_group)
{
   if (p_group)
   {
      printf("groupname: %s\n"\
            " server: %s\n"\
            " username: %s, password:%s\n"\
            " red: gpio%u, gre: gpio%u, blu: gpio%u\n"\
            " display_timeout: %u\n"\
            " last_build_threshold: %u\n",\
            p_group->groupName,
            p_group->server.serverName,
            p_group->server.userName, p_group->server.passWord,
            p_group->gpio.redLed, p_group->gpio.greLed, p_group->gpio.bluLed,
            p_group->displaySuccessTimeout,
            p_group->lastBuildThreshold);
      printAllJobInfo(p_group->p_allJobs);
   }
}

//----------------------------------------------------------------------------
// Init Stuff of All Groups database
//----------------------------------------------------------------------------
void initStuffOfAllGroup(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      if (pthread_mutex_init(&p_group->lockLedSta, NULL))
      {
         printf("Init mutex fail\n");
         exit(1);
      }

      // TODO may be we should allow user config this color through xml file
      p_group->stdLed.disable.color = NON_COLOR ;
      p_group->stdLed.disable.isAnime = false ;

      p_group->stdLed.building.color = YEL_COLOR;
      p_group->stdLed.building.isAnime = true;

      p_group->stdLed.threshold.color = YEL_COLOR;
      p_group->stdLed.threshold.isAnime = false ;

      p_group->stdLed.success.color = BLU_COLOR;
      p_group->stdLed.success.isAnime = false ;

      p_group->stdLed.successNotShow.color = NON_COLOR;
      p_group->stdLed.successNotShow.isAnime = false ;

      p_group->stdLed.fail.color = RED_COLOR;
      p_group->stdLed.fail.isAnime = true;

      p_group->ledStatus.color = WHI_COLOR;
      p_group->ledStatus.isAnime = false;

      // Init CurlTime value
      // TODO: should use Ping or sth like that to get network speed between
      //       current computer and jenkins server, then change this value frequently
      //       I think interval between ajudgement should be 4-5 hour
      //       We can you sigalrm to create timer
      p_group->curlTime.maxTime = 60;
      p_group->curlTime.pollTime = 3;

      p_group->curSta.isBuilding = false;
      p_group->curSta.isSuccess = false;
      p_group->curSta.isThreshold = false;
      p_group->curSta.isAllDisable = true;
   }
}

//----------------------------------------------------------------------------
// Parsing XML file and append data to All group database
//----------------------------------------------------------------------------
bool parseXMLFile(const char* fileName, GroupInfoT** pp_headGroup)
{
   if (g_isVerbose)
   {
      printf("parseXMLFile: %s\n",fileName);
   }
   GroupInfoT* p_curGroup = NULL;
   xmlDoc* doc = NULL;
   xmlNode* rootNode = NULL;
   xmlNode* groupNode = NULL;
	LIBXML_TEST_VERSION

   if ((doc = xmlReadFile(fileName, NULL, 0)) == NULL)
   {
      printf("Can not xmlReadFile\n");
      return false;
   }

   // Get root node of XML file
   rootNode = xmlDocGetRootElement(doc);

   if (rootNode->children == NULL)
   {
      printf("Don't have any group in XML file\n");
      return false;
   }
   else
   {
      // Get start Posision of group database to loop
      p_curGroup = getTailGroup(*pp_headGroup);

      // Iterator to get information of all Groups node in XML file
      for (groupNode = rootNode->children; groupNode; groupNode = groupNode->next)
      {
         if (groupNode->type == XML_ELEMENT_NODE)
         {
            if(!strcmp(groupNode->name, "group"))
            {
               GroupInfoT groupInfoTemp;
               memset(&groupInfoTemp, 0, sizeof(groupInfoTemp));
               if (parseGroupAttr(doc, groupNode, &groupInfoTemp))
               {
                  if (!p_curGroup)
                  {
                     // Allocate memory for head group
                     p_curGroup = malloc(sizeof(GroupInfoT));
                     memset(p_curGroup, 0, sizeof(GroupInfoT));
                     *pp_headGroup = p_curGroup;
                  }
                  else
                  {
                     // Allocate memory for next group
                     p_curGroup->p_nextGroup = malloc(sizeof(GroupInfoT));
                     p_curGroup = p_curGroup->p_nextGroup;
                  }

                  // Get all group data temp to current Group
                  *p_curGroup = groupInfoTemp;
                  p_curGroup->p_nextGroup = NULL;
               }
               else
               {
                  printf("Cannot parse group in XML file\n");
                  return false;
               }
            }
            else
            {
               printf("wrong group XML element: %s\n",groupNode->name);
               return false;
            }
         }
      }
   }

	// Free the document
	xmlFreeDoc(doc);

	// Free the global variables that may have been allocated by the parser.
	xmlCleanupParser();

   return true;
}

//----------------------------------------------------------------------------
// This function is use for parsing all argument in command line
//----------------------------------------------------------------------------
bool parseArgument(int argc, char* argv[])
{
   // TODO should use getopt_long
   int indexArg;
   for (indexArg = 1; indexArg < argc; indexArg++)
   {
      if (!strcmp(argv[indexArg], "-f") && (argc > indexArg + 1))
      {
        g_xmlFile = argv[++indexArg];
      }
      else if (!strcmp(argv[indexArg], "--verbose") ||
               !strcmp(argv[indexArg], "-v"))
      {
         g_isVerbose = true;
      }
      else if (!strcmp(argv[indexArg], "--realled") ||
               !strcmp(argv[indexArg], "-r"))
      {
         g_isCtrlRealLed = true;
      }
      else if (!strcmp(argv[indexArg], "--daemon") ||
               !strcmp(argv[indexArg], "-d"))
      {
         g_isDaemon = true;
      }
      else if (!strcmp(argv[indexArg], "--help") ||
               !strcmp(argv[indexArg], "-h") ||
               !strcmp(argv[indexArg], "help"))
      {
         printf("usage:\n"
                "default xml config file is /opt/jobsJenkinConfig.xml, if we want to change use -f\n"
                "./jenkin_mon\n"
                "./jenkin_mon -f configFILE.xml --verbose --realled --daemon\n"
                "./jenkin_mon -f configFILE.xml -v        -r        -d\n");
         exit(0);
      }
      else
      {
         printf("Wrong argument, to know how to use: please type ./jenkinmon --help\n");
         exit(1);
      }
   }

   return true;
}

//----------------------------------------------------------------------------
// Build full path to files which contains the information of Job
//----------------------------------------------------------------------------
bool buildJobFiles(GroupInfoT* p_headGroup)
{
   char* infoFilesDir = "infoFiles";
#if 0
   // Way 1: to check directory is OK or not
   // Check infoFilesDir exist or not
   DIR* infoFileDir = opendir(infoFilesDir);
   if (infoFileDir)
   {
      if (closedir(infoFileDir))
      {
         printf("There is a problem with infoFiles directory: %s\n", strerror(errno));
         exit(1);
      }
   }
   else
   {
      printf("There is a problem with infoFiles directory: %s\n", strerror(errno));
      exit(1);
   }
#else
   // Way 2: to check directory is OK or not
   struct stat st = {0};
   if (stat(infoFilesDir, &st) == -1)
   {
      if(mkdir(infoFilesDir, (S_IXUSR| S_IRUSR | S_IWUSR) | S_IROTH))
      {
         printf("Can not create directory: %s, error: %s\n", infoFilesDir, strerror(errno));
      }
   }
#endif

   GroupInfoT* p_group = p_headGroup;
   if (p_group)
   {
      u_int32 groupIndex = 0;
      for (; p_group; p_group = p_group->p_nextGroup)
      {
         JobInfoT* p_job = p_group->p_allJobs;
         if (p_job)
         {
            u_int32 jobIndex = 0;
            for (; p_job; p_job = p_job->p_nextJob)
            {
               sprintf(p_job->statusInfoFile, "%s/s%u_%u",
                       infoFilesDir, groupIndex, jobIndex);
               sprintf(p_job->lastBuildInfoFile, "%s/l%u_%u",
                       infoFilesDir, groupIndex, jobIndex);
               jobIndex++;
            }
            groupIndex++;
         }
         else
         {
            printf("Do not have any job in data base\n");
            return false;
         }
      }
   }
   else
   {
      printf("Do not have group in data base\n");
      return false;
   }
   return true;
}

//----------------------------------------------------------------------------
// Get color from status info file
//----------------------------------------------------------------------------
void colorFromFile(char* fileName, char* colorStr, size_t strSize)
{
   char grepCommand[100];
   char colorLine[30];
   sprintf(grepCommand, "grep \"^  \\\"color\" %s", fileName);
   FILE* file = popen(grepCommand, "r");
   if (!file)
   {
      printf("can not openfile: %s, error: %s\n", fileName, strerror(errno));
      return;
   }
   if (fgets(colorLine, sizeof(colorLine), file))
   {
      if (sscanf(colorLine, "  \"color\" : \"%[^\"]", colorStr) != 1)
      {
         printf("get color failed.\n");
      }
   }
   fclose(file);
}

//----------------------------------------------------------------------------
// Get timestamp in second resolution from last build info file
//----------------------------------------------------------------------------
int64 timeStampFromFile(char* fileName)
{
   int64 timeStamp = 0;
   char grepCommand[100];
   char timeStampLine[50];
#if 0
   // Bug in valgrind: "Use of uninitialised value of size 8"
   // because we use timeStampStr variable in later atoll(timeStampStr)
   // We should init for this string
   char timeStampStr[30];
#else
   char timeStampStr[30];
   timeStampStr[0] = 0;
#endif

   sprintf(grepCommand, "grep \"timestamp\" %s", fileName);
   FILE* file = popen(grepCommand, "r");
   if (!file)
   {
      printf("can not openfile: %s, error: %s\n", fileName, strerror(errno));
      return 0;
   }
   if (fgets(timeStampLine, sizeof(timeStampLine), file))
   {
      //printf("timeStampLine: %s\n", timeStampLine);
                               //  \"timestamp\" : 1418372173536
      if (sscanf(timeStampLine, "  \"timestamp\" : %s", timeStampStr) != 1)
      {
         printf("get time stamp failed.\n");
      }
   }
   fclose(file);
   timeStamp = atoll(timeStampStr) / 1000;
   if (g_isVerbose)
   {
      printf("Get timeStamp in ms from file %s: %s (in s :%llu)\n",
              fileName, timeStampStr, timeStamp);
   }

   return timeStamp;
}

//----------------------------------------------------------------------------
// Get current from "date" command timestamp in second resolution
//----------------------------------------------------------------------------
int64 currentTimeStamp(void)
{
   int64 timeStamp = 0;
   struct timespec currentTime;
   if (clock_gettime(CLOCK_REALTIME, &currentTime) == -1)
   {
      printf("Can not get current time: %s\n", strerror(errno));
   }
   timeStamp = (int64)currentTime.tv_sec;
   return timeStamp;
}

//----------------------------------------------------------------------------
// Get led Status from color string
//----------------------------------------------------------------------------
LedInfoT convert2LedInfo(char* colorStr)
{
   LedInfoT led = {NON_COLOR, false};
   char* pAnimeStr = NULL;
   if ((pAnimeStr = strstr(colorStr, "_anime")) != NULL)
   {
      led.isAnime = true;

      //Strip "_anime" string
      memset(pAnimeStr, 0, strlen("_anime"));
   }
   else
   {
      led.isAnime = false;
   }

   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
          (strcmp(colorStr, pColor2Led->colorStr)))
   {
      pColor2Led++;
   }
   led.color = pColor2Led->color;
   return led;
}

//----------------------------------------------------------------------------
// Convert led Status to color string
// Note: need to free pointer to string that are return from this function
//----------------------------------------------------------------------------
#if 0
char* convert2ColorStr(LedInfoT led)
{
   // If we write code like this -> race conditions
   // If two thread call this function at the same time
   // -> colorStr is static -> like global variable
   // -> the same value is return for both thread!
   // XXX: -> should not use static for local variable!

   static char colorStr[20];
   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
          (led.color != pColor2Led->color))
   {
      pColor2Led++;
   }
   strcpy(colorStr, pColor2Led->colorStr);
   if (led.isAnime)
   {
      strcat(colorStr, "_anime");
   }
   return colorStr;
}
#else
void convert2ColorStr(LedInfoT led, char* colorStr, u_int32 strLength)
{
   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
          (led.color != pColor2Led->color))
   {
      pColor2Led++;
   }

   u_int32 tempStrLen = snprintf(colorStr, strLength, "%s", pColor2Led->colorStr);
   if (tempStrLen > strLength)
   {
      printf("Do not enough size to store string\n");
   }
   else
   {
      strLength -= tempStrLen;
   }

   if (led.isAnime)
   {
      if (strLength > strlen("_anime"))
      {
         strcat(colorStr, "_anime");
      }
      else
      {
         printf("Do not enough size to store string\n");
      }
   }
}
#endif

//----------------------------------------------------------------------------
// Convert RGB gpio status to color string
//----------------------------------------------------------------------------
char* convertRgb2ColorStr(GpioStatusE r, GpioStatusE g, GpioStatusE b)
{
   Color2LedInfoT* pColor2Led = C2LInfo;
   pColor2Led += 2; //if (r,g,b) = (OF, OF, OF), This function should return "noColor"
                    //instead of "notbuilt" or "disabled",
                    // => ignore these 2 lines in searching
   while ((pColor2Led->color != NON_COLOR) &&
          ((r != pColor2Led->r) || (g != pColor2Led->g) || (b != pColor2Led->b)))
   {
      pColor2Led++;
   }
   return pColor2Led->colorStr;
}

//----------------------------------------------------------------------------
// Get led Status Info from file
//----------------------------------------------------------------------------
LedInfoT ledInfoFromfile(char* fileName)
{
   char colorStr[20];
   colorFromFile(fileName, colorStr, sizeof(colorStr));
   LedInfoT ledInfo = convert2LedInfo(colorStr);
   if (g_isVerbose)
   {
      convert2ColorStr(ledInfo, colorStr, 20);
      printf("Get color from file %s: %s\n", fileName, colorStr);
   }
   return ledInfo;
}

//----------------------------------------------------------------------------
// Read current value of GPIO
//----------------------------------------------------------------------------
GpioStatusE readGPIOValue(u_int8 pin)
{
   if (g_isCtrlRealLed)
   {
      GpioStatusE value;
      char catCommand[200];
      char valueStr[10];
      sprintf(catCommand, "cat /sys/class/gpio/gpio%u/value", pin);
      FILE* file;
      if (!(file = popen(catCommand, "r")))
      {
         printf("Can not read value of gpio: %s\n", catCommand);
         return;
      }
      if (fgets(valueStr, sizeof(valueStr), file))
      {
         value = atoi(valueStr);
      }
      fclose(file);
      return value;
   }
   else
   {
      return 0;
   }
}

//----------------------------------------------------------------------------
// Set state to GPIO after checking current state
//----------------------------------------------------------------------------
void setGPIOValue(u_int8 pin, GpioStatusE state)
{
   if (g_isCtrlRealLed)
   {
      if (state != readGPIOValue(pin))
      {
         char echoCommand[200];
         sprintf(echoCommand,
               "echo %d > /sys/class/gpio/gpio%u/value", state, pin);
         FILE* file;
         if (!(file = popen(echoCommand, "r")))
         {
            printf("Can not set value to gpio: %s\n", echoCommand);
            return;
         }
         fclose(file);
      }
   }
}

//----------------------------------------------------------------------------
// Set state to GPIO without checking current state
//----------------------------------------------------------------------------
void setGPIOValueNoCheck(u_int8 pin, GpioStatusE state)
{
   if (g_isCtrlRealLed)
   {
      char echoCommand[200];
      sprintf(echoCommand,
            "echo %d > /sys/class/gpio/gpio%u/value", state, pin);
      FILE* file;
      if (!(file = popen(echoCommand, "r")))
      {
         printf("Can not set value to gpio: %s\n", echoCommand);
         return;
      }
      fclose(file);
   }
}

//----------------------------------------------------------------------------
// Control led only by setting value to GPIO
//----------------------------------------------------------------------------
void ledCtrl(ColorE color, GpioStatusE gpioState, LedGpioT gpioLed, char* stuffInfoStr)
{
   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
         (color != pColor2Led->color))
   {
      pColor2Led++;
   }

   GpioStatusE r = OF;
   GpioStatusE g = OF;
   GpioStatusE b = OF;

   if (gpioState == ON)
   {
      r = pColor2Led->r;
      g = pColor2Led->g;
      b = pColor2Led->b;
   }

   // Set value for GPIO -> control Led
   if (g_isCtrlRealLed)
   {
      setGPIOValueNoCheck(gpioLed.redLed, r);
      setGPIOValueNoCheck(gpioLed.greLed, g);
      setGPIOValueNoCheck(gpioLed.bluLed, b);
   }
   else
   {
      printf("\nGroup %s's LED color: %s <=> red-green-blue: %d-%d-%d r-g-b:%d-%d-%d\n",
             stuffInfoStr, convertRgb2ColorStr(r,g,b),
             gpioLed.redLed, gpioLed.greLed, gpioLed.bluLed,
             r, g, b);
   }
}

//----------------------------------------------------------------------------
// Build multiple threads to Evaluate Color for each Group
// Each Group has 1 threads to evaluate group's color
//----------------------------------------------------------------------------
bool buildEvalGrpColorTheads(GroupInfoT* p_headGroup)
{
   bool areAllOk = true;
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      if (pthread_create(&p_group->evalColorThread, NULL, evalGrpColorPoll, p_group))
      {
         areAllOk = false;
         break;
      }
   }
   return areAllOk;
}

//----------------------------------------------------------------------------
// Evaluate Color for Group
//----------------------------------------------------------------------------
void* evalGrpColorPoll(void* arg)
{
   GroupInfoT* p_group = (GroupInfoT*)arg;
   u_int32 curlCmdSize = 10000;
   char* pCurlCmd = malloc(curlCmdSize * sizeof(*pCurlCmd));
   if (!buildCurlCmd(p_group, pCurlCmd, curlCmdSize))
   {
      printf("Can not build curl Command\n");
      exitNow();
   }
   else
   {
      if (g_isVerbose)
      {
         printf("Group %s's Curl command:\n%s\n", p_group->groupName, pCurlCmd);
      }
   }

   while (1)
   {
      pthread_mutex_lock(&g_terminateLock);
      bool tempTerminate = g_terminateAll;
      pthread_mutex_unlock(&g_terminateLock);
      if (tempTerminate)
      {
         break;
      }

      if (executeCurlCmd(pCurlCmd))
      {
         evaluateColor(p_group);
         sleep(p_group->curlTime.pollTime);
      }
   }

   free(pCurlCmd);
   return 0;
}

//----------------------------------------------------------------------------
// Build Curl command string
//----------------------------------------------------------------------------
bool buildCurlCmd(GroupInfoT* p_group, char* curlCommand, u_int32 curlCommandSize)
{
   u_int32 remainLen = curlCommandSize;
   u_int32 len;

#if USE_ANY_AUTHORIZED_IN_CURL
   len = snprintf(curlCommand, remainLen, "curl --silent --max-time %d --anyauth ",
                  p_group->curlTime.maxTime);
#else
   len = snprintf(curlCommand, remainLen, "curl --silent --max-time %d -u %s:%s ",
                  p_group->curlTime.maxTime, p_group->server.userName, p_group->server.passWord);
#endif
   if (len >= remainLen)
   {
      return false;
   }
   else
   {
      remainLen -= len;
   }

   JobInfoT* p_job = p_group->p_allJobs;
   if (p_job)
   {
      for (; p_job; p_job = p_job->p_nextJob)
      {
         // Build command to get status of Job
         char* pStatusCmd = malloc(remainLen * sizeof(*pStatusCmd));
         len = snprintf(pStatusCmd, remainLen,
                        " %s%s%s/api/json?pretty=true\\&"\
                        "tree=name,color -o %s",
                        p_group->server.serverName, p_job->jobPath, p_job->jobName,
                        p_job->statusInfoFile);
         if (len >= remainLen)
         {
            free(pStatusCmd);
            return false;
         }
         else
         {
            remainLen -= len;
            strcat(curlCommand, pStatusCmd);
            free(pStatusCmd);
         }

         // Build command to get last build information of Job
         char* pLastBuildCmd = malloc(remainLen * sizeof(*pLastBuildCmd));
         len = snprintf(pLastBuildCmd, remainLen,
                       " %s%s%s/lastBuild/api/json?pretty=true\\&"\
                       "tree=fullDisplayName,id,timestamp,result -o %s",
                       p_group->server.serverName, p_job->jobPath, p_job->jobName,
                       p_job->lastBuildInfoFile);
         if (len >= remainLen)
         {
            free(pLastBuildCmd);
            return false;
         }
         else
         {
            remainLen -= len;
            strcat(curlCommand, pLastBuildCmd);
            free(pLastBuildCmd);
         }
      }

      // Add done string
      char* doneStr = malloc(remainLen * sizeof(*doneStr));
      len = snprintf(doneStr, remainLen,
                     ";echo Finish curl from jenkin server: %s", p_group->server.serverName);
      if (len >= remainLen)
      {
         free(doneStr);
         return false;
      }
      else
      {
         strcat(curlCommand, doneStr);
         free(doneStr);
      }
   }
   else
   {
      printf("Do not have any job in data base\n");
      return false;
   }

   return true;
}

//----------------------------------------------------------------------------
// Execute curl command to get information about a groups from jenkin server
//----------------------------------------------------------------------------
bool executeCurlCmd(char* curlCommand)
{
   // TODO: should use CURL api function instead of execute curl command in shell
   char curlDoneStr[100];
   curlDoneStr[0] = 0; //To void valgrind error
                       //"Conditional jump or move depends on uninitialised value(s)"
                       //If curlDoneStr is not initialized and not modified before printf
                       //, we will prints many things we do not expect!
   FILE* file;
   // XXX : by using system() function, we can not receive SIGTERM or
   //       SIGINT signal by press "Ctrl + C"
   //       -> solution: use popen() function
   if ((file = popen(curlCommand, "r")) == NULL)
   {
      printf("Can not execute command: %s\n", curlCommand);
      return false;
   }

   while (fgets(curlDoneStr, sizeof(curlDoneStr), file) != NULL)
   {
      pthread_mutex_lock(&g_terminateLock);
      bool tempTerminate = g_terminateAll;
      pthread_mutex_unlock(&g_terminateLock);
      if (tempTerminate)
      {
         break;
      }
   }

   fclose(file);
   if (g_isVerbose)
   {
      printf("%s\n", curlDoneStr);
   }
   return true;
}

//----------------------------------------------------------------------------
// Evaluate Color for group
//----------------------------------------------------------------------------
void evaluateColor(GroupInfoT* p_group)
{
   // evaluate Group Status base on information store in status files and last
   // last build status file
   evalGroupStatus(p_group);

   // evaluate Led status base on Current Group Status information and
   // last group Status information
   evalLedStatus(p_group);

   if (g_isVerbose)
   {
      char str[100];
      char colorStr[20];

      pthread_mutex_lock(&p_group->lockLedSta);
      convert2ColorStr(p_group->ledStatus, colorStr, 20);
      pthread_mutex_unlock(&p_group->lockLedSta);

      snprintf(str, 100, "%s - %s - %s- %s",
               (p_group->curSta.isAllDisable) ?  "Disable"   : " ",
               (p_group->curSta.isThreshold)  ?  "Threshold" : " ",
               (p_group->curSta.isBuilding)   ?  "Building"  : "Not building ",
               (p_group->curSta.isSuccess)    ?  "Success"   : "False ");

      printf("\nGroup %s\ngroup status:%s; led status: %s\n\n",
             p_group->groupName, str, colorStr);

   }
}

//----------------------------------------------------------------------------
// evaluate Group Status
//----------------------------------------------------------------------------
void evalGroupStatus(GroupInfoT* p_group)
{
   p_group->preSta = p_group->curSta;
   p_group->curSta.isBuilding = false;
   p_group->curSta.isSuccess = true;
   p_group->curSta.isThreshold = false;
   p_group->curSta.isAllDisable = true;
   JobInfoT* p_job = NULL;
   bool isJobThreshold;

   for (p_job = p_group->p_allJobs; p_job; p_job = p_job->p_nextJob)
   {
      LedInfoT jobLedInfo = ledInfoFromfile(p_job->statusInfoFile);
      p_group->curSta.isAllDisable = p_group->curSta.isAllDisable &&
         ((jobLedInfo.color == NO_BUILT) || (jobLedInfo.color == DISABLED));
      if ((jobLedInfo.color != NO_BUILT) &&
            (jobLedInfo.color != DISABLED))
      {
         p_group->curSta.isSuccess = p_group->curSta.isSuccess &&
                                     (jobLedInfo.color == BLU_COLOR);

         p_group->curSta.isBuilding = p_group->curSta.isBuilding ||
                                      jobLedInfo.isAnime;

         int64 jobTime = timeStampFromFile(p_job->lastBuildInfoFile);
         int64 curTime = currentTimeStamp();
         isJobThreshold = (curTime - jobTime) > (int64)p_group->lastBuildThreshold;
         p_group->curSta.isThreshold = p_group->curSta.isThreshold || isJobThreshold;
      }
   }
}

//----------------------------------------------------------------------------
// assign Group's Led Status
//----------------------------------------------------------------------------
void assignGrpLedStatus(GroupInfoT* p_group, LedInfoT ledInfo)
{
   pthread_mutex_lock(&p_group->lockLedSta);
   p_group->ledStatus = ledInfo;
   pthread_mutex_unlock(&p_group->lockLedSta);
}

//----------------------------------------------------------------------------
// Evaluate Group's Led Status
//----------------------------------------------------------------------------
void evalLedStatus(GroupInfoT* p_group)
{
   if (p_group->curSta.isAllDisable)
   {
      assignGrpLedStatus(p_group, p_group->stdLed.disable);
   }
   else
   {
      if (p_group->curSta.isBuilding)
      {
         assignGrpLedStatus(p_group, p_group->stdLed.building);
      }
      else
      {
         if (p_group->curSta.isSuccess)
         {
            if (p_group->curSta.isThreshold)
            {
               assignGrpLedStatus(p_group, p_group->stdLed.threshold);
            }
            else
            {
               if (!p_group->preSta.isAllDisable &&
                   !p_group->preSta.isBuilding   &&
                   p_group->preSta.isSuccess     &&
                   !p_group->preSta.isThreshold)
               {
                  //Previous status is full success as current status
                  if (p_group->needToCheckTimeStamp)
                  {
                     //Check timestamp to turn off Led
                     int64 curTime = currentTimeStamp();
                     if ((curTime - p_group->lastSuccessTimeStamp) >
                         (int64)p_group->displaySuccessTimeout)
                     {
                        assignGrpLedStatus(p_group, p_group->stdLed.successNotShow);

                        // LED is turned off -> don't need to check timestamp anymore
                        p_group->needToCheckTimeStamp = false;
                     }
                  }
               }
               else
               {
                  //First time full success occur -> Store timestamp
                  p_group->lastSuccessTimeStamp = currentTimeStamp();
                  p_group->needToCheckTimeStamp = true;

                  assignGrpLedStatus(p_group, p_group->stdLed.success);
               }
            }
         }
         else
         {
            assignGrpLedStatus(p_group, p_group->stdLed.fail);
         }
      }
   }
}

//----------------------------------------------------------------------------
// Build threads to control led for each group'
//----------------------------------------------------------------------------
bool buildCtrlGrpLedThreads(GroupInfoT* p_headGroup)
{
   bool areAllOk = true;
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      if (pthread_create(&p_group->ctrlLedThread, NULL, ctrlGrpLedPoll, p_group))
      {
         areAllOk = false;
         break;
      }
   }
   return areAllOk;
}

//----------------------------------------------------------------------------
// Poll to control all led for group
//----------------------------------------------------------------------------
void* ctrlGrpLedPoll(void *arg)
{
   GroupInfoT* p_group = (GroupInfoT*)arg;
   LedInfoT preLedSta;
   LedInfoT curLedSta;
   GpioStatusE gpioSta;

   preLedSta.color = NON_COLOR;
   preLedSta.isAnime = false;

   while (1)
   {
      pthread_mutex_lock(&g_terminateLock);
      bool tempTerminate = g_terminateAll;
      pthread_mutex_unlock(&g_terminateLock);
      if (tempTerminate)
      {
         break;
      }

      pthread_mutex_lock(&p_group->lockLedSta);
      curLedSta = p_group->ledStatus;
      pthread_mutex_unlock(&p_group->lockLedSta);

      // Check if previous Led status and current Led status is the same or not
      if ((preLedSta.color == curLedSta.color) &&
          (preLedSta.isAnime == curLedSta.isAnime))
      {
         if (curLedSta.isAnime)
         {
            gpioSta = (gpioSta == ON) ? OF : ON;
            ledCtrl(curLedSta.color, gpioSta, p_group->gpio, p_group->groupName);
         }
         else
         {
            char colorStr[20];
            convert2ColorStr(curLedSta, colorStr, 20);
            printf("\nGroup %s's LED color: %s , led will not blink and led color is the same as before\n",
                   p_group->groupName, colorStr);
         }
      }
      else
      {
         gpioSta = ON;
         ledCtrl(curLedSta.color, gpioSta, p_group->gpio, p_group->groupName);
         preLedSta = curLedSta;
      }

      sleep(g_ledAnimeTime);
   }
   return 0;
}

//----------------------------------------------------------------------------
// Wait until all threads have been stopped
//----------------------------------------------------------------------------
void waitAllThreadsStop(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      if (pthread_join(p_group->evalColorThread, NULL) ||
          pthread_join(p_group->ctrlLedThread, NULL))
      {
         printf("Can not join threads\n");
         exit(1);
      }
   }
}

//----------------------------------------------------------------------------
// Cleanup all Group information
//----------------------------------------------------------------------------
void cleanAllGroupInfo(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_tempGroup = NULL;
   while (p_headGroup)
   {
      p_tempGroup = p_headGroup;
      p_headGroup = p_headGroup->p_nextGroup;

      // Clean All Jobs in Group
      JobInfoT* p_headJob = p_tempGroup->p_allJobs;
      JobInfoT* p_tempJob = NULL;
      while (p_headJob)
      {
         p_tempJob = p_headJob;
         p_headJob = p_headJob->p_nextJob;
         free(p_tempJob->jobPath);
         free(p_tempJob->jobName);
         free(p_tempJob);
      }

      free(p_tempGroup->groupName);
      free(p_tempGroup->server.serverName);
      free(p_tempGroup->server.userName);
      free(p_tempGroup->server.passWord);
      pthread_mutex_destroy(&p_tempGroup->lockLedSta);
      free(p_tempGroup);
   }
}
//----------------------------------------------------------------------------
// Main function
//----------------------------------------------------------------------------
int main(int argc, char *argv[])
{
   // Parse Argument from command line
   if (!parseArgument(argc, argv))
   {
      printf("Can not parse command line!\n");
      exit(1);
   }

   // Daemonize
   // Reference: http://codingfreak.blogspot.com/2012/03/daemon-izing-process-in-linux.html
   if (g_isDaemon)
   {
      switch (fork())
      {
         case 0:
            break;
         case -1:
            fprintf(stderr, "fork failed");
            exit(1);
         default:
            exit(0); // Exit parent process
      }
      if (setsid() == -1) //Create new sesion containing a single (new) process group
      {
         fprintf(stderr, "setsid failed");
         exit(1);
      }
      close(0); //Close standard input stdin
      close(1); //Close standard outout stdout
      close(2); //Close standard error stderr
   }

	// Make sure SIGCHLD is not SIG_IGN
	while (signal(SIGCHLD, sig_chld) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}

   if (pthread_mutex_init(&g_terminateLock, NULL))
   {
      printf("Can not init mutex for terminate flag\n");
      exit(1);
   }

	// register SIGTERM handle
	while (signal(SIGTERM, sig_term) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}

	// register SIGINT hanlde
	while (signal(SIGINT, sig_term) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}

   GroupInfoT* p_allGroups = NULL;

   // Parse XML file
   if (!parseXMLFile(g_xmlFile, &p_allGroups))
   {
      printf("Can not parse XML file\n");
      exit(1);
   }

   // Build files to store data get from curl commands for each jobs
   if (!buildJobFiles(p_allGroups))
   {
      printf("Can not buildJobFiles\n");
      exit(1);
   }

   printAllGroupInfo(p_allGroups);

   // Init Stuff of All Groups database
   initStuffOfAllGroup(p_allGroups);

   // Init all LED of All groups
   initAllGroupLed(p_allGroups);

   // Build thread to Evaluate Color for each Group
   // Each Group will have one thread to Evaluate Group's Color
   if (!buildEvalGrpColorTheads(p_allGroups))
   {
      printf("Can not build evalute color theads\n");
   }

   // Build thead to control Group's Led
   // Each Group will have one thread to control its Led
   if (!buildCtrlGrpLedThreads(p_allGroups))
   {
      printf("Can not build control led threads\n");
   }

#if 0
   // DEAD lock occur if we write code in this way
   // + Assume SIGINT occur when g_terminateLock is locked (this situation will occur very frequently
   // because main thread executed these line of code almost time)
   // + Main thread jump to sigterm() function.
   // + in sigterm() function, we will wait for g_terminateLock is unlock.
   // + But g_terminateLock can be unlock anymore, because it is locked in main thread itself!
   // => deadlock

   while (1)
   {
      //Main thread will wait until g_terminateAll is true
      pthread_mutex_lock(&g_terminateLock);
      bool tempTerminate = g_terminateAll;
      pthread_mutex_unlock(&g_terminateLock);
      if (tempTerminate)
      {
         break;
      }
   }
#endif

   //Waiting for all evaluated Color Threads and Led Control Threads stop
   waitAllThreadsStop(p_allGroups);

   // Clean all Group and job database /free data...
   cleanAllGroupInfo(p_allGroups);

   pthread_mutex_destroy(&g_terminateLock);

	return 0;
}
