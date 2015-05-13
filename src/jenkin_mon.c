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
//* Version v1.1:
//   + Design:
//      - Color Thread: One thread
//         . execute all Curls command to get information of all jobs
//         . then read files and evaluate color
//         . use Mutex to protect evaluated color variable
//      - Led Thread: One thread to control led
//         . Read data from Evaluated color and control GPIO   
//         . Use mutex to protect evaluated color variable
//    + Disadvantage: 
//       - if one curls commands stuck when getting information from one server 
//         ,we can not getting information of another server
//    + TODO:
//         - add "--maxtime" option into each curl command
//         - maybe we divide theads base on group, each groups will have one thead for execute
//           curl and evaluate color, one thread to control led group
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
           
// Animed Led Interval
#define ANIME_LED_INTERVAL 1

// Option to use authorized account to get info from jenkin server or not
#define USE_ANY_AUTHORIZED_IN_CURL 1

//----------------------------------------------------------------
// Global variable
//----------------------------------------------------------------
GroupInfoT* p_allGroups = NULL;

char* pathInfoFiles = "/opt/infoFiles";
char* xmlFile = "/opt/jobsJenkinConfig.xml";

// Option to control log
bool isVerbose = false;

// Option to control led
bool isControlRealLed = true;
bool isAllowAnime = true;
bool isDaemon = false;

/* Termination flag */
static int terminate = 0;
 
static void sig_term(int isig)
{
	terminate = 1;
}

static void sig_chld(int sig)
{
	/*
	 * This function will never be executed, however it is
	 * needed to make sure SIGCHLD is not set to SIG_IGN,
	 * in which case we can't wait() on any child.
	 */
}

static void exitNow()
{
   // TODO: mutex for this terminate? yes -> we need to do this
   terminate = 1;
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
#if 0
   GroupInfoT* p_curGroup = p_headGroup;
   while (p_curGroup->p_nextGroup)
   {
      p_curGroup = p_curGroup->p_nextGroup;
   }
#else
   GroupInfoT* p_curGroup = NULL;
   for (p_curGroup = p_headGroup;
        p_curGroup->p_nextGroup;
        p_curGroup = p_curGroup->p_nextGroup);
#endif
   return p_curGroup;
}

void printAllJobInfo(JobInfoT* p_jobHead)
{
   JobInfoT* p_job = NULL;
   for (p_job = p_jobHead; p_job; p_job = p_job->p_nextJob)
   {
      printJobInfo(p_job);
   }
}

void printJobInfo(JobInfoT* p_job)
{
   if (p_job)
   {
      printf("job name: %s\n"\
             " job path: %s\n"\
             " status file: %s\n"\
             " last status file: %s\n",\
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
         if (!strcmp(jobAttrNode->name, "jobpath"))
         {
            p_job->jobPath =
               xmlNodeListGetString(doc, jobAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(jobAttrNode->name, "jobname"))
         {
            p_job->jobName = 
               xmlNodeListGetString(doc, jobAttrNode->xmlChildrenNode, 1);
         }
         else
         {
            printf("Wrong job attribute: %s\n", jobAttrNode->name);
            return false;
         }
      }
   }
   if (isVerbose)
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
         if (!strcmp(groupAttrNode->name, "groupname"))
         {
            p_group->groupName =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "server"))
         {
            p_group->server.serverName =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "username"))
         {
            p_group->server.userName =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "password"))
         {
            p_group->server.passWord =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "red_led"))
         {
            p_group->redLed =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "green_led"))
         {
            p_group->greLed =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "blue_led"))
         {
            p_group->bluLed =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "display_timeout"))
         {
            p_group->displaySuccessTimeout =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "last_build_threshold"))
         {
            p_group->lastBuildThreshold=
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
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
   if (isVerbose)
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
      p_group->ledStatus.color = WHI_COLOR;
      p_group->ledStatus.isAnime = false;
      ledControl(p_group->ledStatus,
                 p_group->redLed, p_group->greLed, p_group->bluLed);
      p_group->ledStatus.color = NON_COLOR;
      ledControl(p_group->ledStatus,
                 p_group->redLed, p_group->greLed, p_group->bluLed);
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
            p_group->redLed, p_group->greLed, p_group->bluLed,
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
      p_group->isFirstDisplaySuccess = false;
      p_group->lastSuccessTimeStamp = currentTimeStamp();

      // Init CurlTime value
      // TODO: should use Ping or sth like that to get network speed between
      //       current computer and jenkins server, then change this value frequently
      //       I think interval between ajudgement should be 4-5 hour
      p_group->curlTime.maxTime = 60;
      p_group->curlTime.pollTime = 3;

      p_group->curSta.isAnime = false; 
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
   if (isVerbose)
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
   int indexArg; 
   for (indexArg = 1; indexArg < argc; indexArg++)
   {
      if (!strcmp(argv[indexArg], "-f") && (argc > indexArg + 1)) 
      {
        xmlFile = argv[++indexArg]; 
      }
      else if (!strcmp(argv[indexArg], "--verbose") ||
               !strcmp(argv[indexArg], "-v"))
      {
         isVerbose = true;
      }
      else if (!strcmp(argv[indexArg], "--realled") ||
               !strcmp(argv[indexArg], "-r"))
      {
         isControlRealLed = true;
      }
      else if (!strcmp(argv[indexArg], "--allowanime") ||
               !strcmp(argv[indexArg], "-a"))
      {
         isAllowAnime = true; 
      }
      else if (!strcmp(argv[indexArg], "--daemon") ||
               !strcmp(argv[indexArg], "-d"))
      {
         isDaemon = true;
      }
      else if (!strcmp(argv[indexArg], "--currentPath") ||
               !strcmp(argv[indexArg], "-c"))
      {
         pathInfoFiles = "./infoFiles";
      }
      else if (!strcmp(argv[indexArg], "--help") ||
          !strcmp(argv[indexArg], "-h") ||
          !strcmp(argv[indexArg], "help"))
      {
         printf("usage:\n"
                "default xml config file is /opt/jobsJenkinConfig.xml, if we want to change use -f\n"
                "default infoFiles directory is /opt/infoFiles, if we want to change to current path -> use -c\n"
                "./jenkin_mon\n"
                "./jenkin_mon -f configFILE.xml --currentPath --verbose --realled --allowanime --daemon\n"
                "./jenkin_mon -f configFILE.xml -c            -v        -r        -a           -d\n");
         exit(0);
      }
   }
   
   return true;
}

bool buildJobFiles(GroupInfoT* p_headGroup)
{
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
                       pathInfoFiles, groupIndex, jobIndex);
               sprintf(p_job->lastBuildInfoFile, "%s/l%u_%u",
                       pathInfoFiles, groupIndex, jobIndex);
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
   if (isVerbose)
   {
      printf("get color from file: %s\n", fileName);
   }
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
   if (isVerbose)
   {
      printf("get timestamp from file: %s\n", fileName);
   }
   char grepCommand[100];
   char timeStampLine[50];
   char timeStampStr[30];
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

   return timeStamp;
}

//----------------------------------------------------------------------------
// Get current from "date" command timestamp in second resolution 
//----------------------------------------------------------------------------
int64 currentTimeStamp(void)
{
   int64 timeStamp = 0;
#if 0
   char* dateCommand = "date +%s";
   char timeStampStr[30];
   FILE* file = popen(dateCommand, "r");
   if (!file)
   {
      printf("can not get timestamp\n");
      return timeStamp;
   }
   if (fgets(timeStampStr, sizeof(timeStampStr), file))
   {
      timeStamp = atoll(timeStampStr);
   }
   fclose(file);
#else
   struct timespec currentTime;
   if (clock_gettime(CLOCK_REALTIME, &currentTime) == -1)
   {
      printf("Can not get current time: %s\n", strerror(errno));
   }
   timeStamp = (int64)currentTime.tv_sec;
#endif
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
//----------------------------------------------------------------------------
char* convert2ColorString(LedInfoT led)
{
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

//----------------------------------------------------------------------------
// Get led Status Info from file
//----------------------------------------------------------------------------
LedInfoT ledInfoFromfile(char* fileName)
{
   char colorStr[20];
   colorFromFile(fileName, colorStr, sizeof(colorStr));
   LedInfoT ledInfo = convert2LedInfo(colorStr);
   if (isVerbose)
   {
      printf("color: %d, isAnime %d\n", ledInfo.color, ledInfo.isAnime);
   }
   return ledInfo;
}

//----------------------------------------------------------------------------
// Read current value of GPIO
//----------------------------------------------------------------------------
GpioStatusE readGPIOValue(u_int8 pin)
{
   if (isControlRealLed)
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
   if (isControlRealLed)
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
   if (isControlRealLed)
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
// Control led by read/set value to GPIO
//----------------------------------------------------------------------------
void ledControl(LedInfoT led, u_int8 red, u_int8 green, u_int8 blue)
{
   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
         (led.color != pColor2Led->color))
   {
      pColor2Led++;
   }

   GpioStatusE r = OF;
   GpioStatusE g = OF;
   GpioStatusE b = OF;

   // Decide next state for Led
   if (isAllowAnime && led.isAnime)
   {
      // Specify current state for all Leds
      GpioStatusE currentState;
      if (pColor2Led->r == ON)
      {
         currentState = readGPIOValue(red);
      }
      else if (pColor2Led->g == ON)
      {
         currentState = readGPIOValue(green);
      }
      else if (pColor2Led->b == ON)
      {
         currentState = readGPIOValue(blue);
      }
      else
      {
         currentState = readGPIOValue(red);
      }

      // Decide next state for all Leds
      if (pColor2Led->r == ON)
      {
         r = !currentState;
      }

      if (pColor2Led->g == ON)
      {
         g = !currentState;
      }

      if (pColor2Led->b == ON)
      {
         b = !currentState;
      }
   }
   else
   {
      r = pColor2Led->r;
      g = pColor2Led->g;
      b = pColor2Led->b;
   }

   // Set value for GPIO -> control Led
   if (isControlRealLed)
   {
      setGPIOValueNoCheck(red  , r);
      setGPIOValueNoCheck(green, g);
      setGPIOValueNoCheck(blue , b);
   }
   else
   {
      printf("red-green-blue: %d-%d-%d r-g-b:%d-%d-%d\n", red, green, blue, r, g, b);  
   }
}

//----------------------------------------------------------------------------
// Build multiple threads to Evaluate Color for each Group
// Each Group has 1 threads to evaluate group's color
//----------------------------------------------------------------------------
bool buildEvalGrpColorTheads(GroupInfoT* p_headGroup)
{
   bool AreAllOk = true;
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      if (pthread_create(&p_group->evalColorThread, NULL, evalGrpColorPoll, p_group)) 
      {
         AreAllOk = false;
         break;
      }
   }
   return AreAllOk;
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
   while (!terminate)
   {
      if (executeCurlCmd(pCurlCmd))
      {
         evaluateColor(p_group);
         sleep(p_group->curlTime.pollTime);
      }
   }
   free(pCurlCmd);
   return NULL;
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
                     ";echo done %s", p_group->server.serverName);
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
}

//----------------------------------------------------------------------------
// Execute curl command to get information about a groups from jenkin server
//----------------------------------------------------------------------------
bool executeCurlCmd(char* curlCommand)
{
   char doneString[100];
   FILE* file;
   // XXX : by using system() function, we can not receive SIGTERM or
   //       SIGINT signal by press "Ctrl + C"
   //       -> solution: use popen() function
   if ((file = popen(curlCommand, "r")) == NULL)
   {
      printf("Can not execute command: %s\n", curlCommand);
      return false;
   }
   while ((fgets(doneString, sizeof(doneString), file) != NULL) && (!terminate));
   fclose(file);
   if (isVerbose)
   {
      printf("doneString: %s for server: %s\n", doneString);
   }
}

//----------------------------------------------------------------------------
// evaluate Color for group
//----------------------------------------------------------------------------
void evaluateColor(GroupInfoT* p_group)
{
   // evaluate Group Status base on information store in status files and last
   // last build status file
   evalGroupStatus(p_group);

   // evaluate Led status base on Current Group Status information and
   // last group Status information
   evalLedStatus(p_group);
}

//----------------------------------------------------------------------------
// evaluate Group Status
//----------------------------------------------------------------------------
void evalGroupStatus(GroupInfoT* p_group)
{
   p_group->preSta = p_group->curSta;
   p_group->curSta.isAnime = false; 
   p_group->curSta.isSuccess = true;
   p_group->curSta.isThreshold = false;
   p_group->curSta.isAllDisable = true;
   JobInfoT* p_job = NULL;

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

         p_group->curSta.isAnime = p_group->curSta.isAnime || jobLedInfo.isAnime; 

         int64 jobTime = timeStampFromFile(p_job->lastBuildInfoFile);
         int64 curTime = currentTimeStamp();
         p_group->curSta.isThreshold = p_group->curSta.isThreshold || 
                           ((curTime - jobTime) > (int64)p_group->lastBuildThreshold); 
      }
   }
}

//----------------------------------------------------------------------------
// Evaluate Group's Led Status
//----------------------------------------------------------------------------
void evalLedStatus(GroupInfoT* p_group)
{
   if (p_group->curSta.isAllDisable)
   {
      p_group->ledStatus.color = NON_COLOR;
      p_group->ledStatus.isAnime = false;
   }
   else
   {
      if (p_group->curSta.isThreshold)
      {
         p_group->ledStatus.color = YEL_COLOR;
         p_group->ledStatus.isAnime = false;
      }
      else
      {
         if (p_group->curSta.isAnime)
         {
            p_group->ledStatus.color = YEL_COLOR;
            p_group->ledStatus.isAnime = true;
         }
         else
         {
            if (p_group->curSta.isSuccess)
            {
               if (!p_group->preSta.isAllDisable &&
                   !p_group->preSta.isThreshold &&
                   !p_group->preSta.isAnime &&
                   p_group->preSta.isSuccess)
               {
                  //Previous status is full success as current status
                  //Check timestamp to turn off Led
                  int64 curTime = currentTimeStamp();
                  if ((curTime - p_group->lastSuccessTimeStamp) >
                      (int64)p_group->displaySuccessTimeout)
                  {
                     p_group->ledStatus.color = NON_COLOR;
                     p_group->ledStatus.isAnime = false;
                  }
               }
               else
               {
                  //First time full success occur
                  //Store timestamp
                  p_group->lastSuccessTimeStamp = currentTimeStamp();
                  p_group->ledStatus.color = BLU_COLOR;
                  p_group->ledStatus.isAnime = false;
               }
            }
            else
            {
               p_group->ledStatus.color = RED_COLOR;
               p_group->ledStatus.isAnime = true;
            }
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
   for (p_group = p_headGroup; p_group; p_group->p_nextGroup)
   {
      if (!pthread_create(&p_group->ctrlLedThread, NULL, ctrlGrpLedPoll, p_group))
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
   while (!terminate)
   {
      ledControl(p_group->ledStatus,
                 p_group->redLed, p_group->greLed, p_group->bluLed);
      sleep(ANIME_LED_INTERVAL);
   }
   return NULL;
}

//----------------------------------------------------------------------------
// This function is use for parsing all argument in command line
//----------------------------------------------------------------------------
int main(int argc, char *argv[])
{
	int job_count = 0, i;
	struct thread_info *job_thread_tmp;
	struct job_data *job_data_tmp;
	char date[8];
	memset(date, 0, sizeof(date));

   // Parse Argument from command line
   if (!parseArgument(argc, argv))
   {
      printf("Can not parse command line!\n");
      exit(0);
   }

   // Daemonize
   // Reference: http://codingfreak.blogspot.com/2012/03/daemon-izing-process-in-linux.html
   if (isDaemon)
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

	/* Make sure SIGCHLD is not SIG_IGN */
	while (signal(SIGCHLD, sig_chld) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}

	/* Catch SIGTERM */
	while (signal(SIGTERM, sig_term) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}

	/* Catch SIGINT */
	while (signal(SIGINT, sig_term) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}
   
   // Parse XML file
   if (!parseXMLFile(xmlFile, &p_allGroups))
   {
      printf("Can not parse XML file\n");
      exit(0);
   }

   // Build files to store data get from curl commands for each jobs
   if (!buildJobFiles(p_allGroups))
   {
      printf("Can not buildJobFiles\n");
      exit(0);
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

   //Main thread will wait until terminated!
   while(!terminate);

   //Waiting for all evaluated Color Threads and Led Control Threads stop
   GroupInfoT* p_group = NULL;
   for (p_group = p_allGroups; p_group; p_group->p_nextGroup)
   {
      if (pthread_join(p_group->evalColorThread, NULL) ||
          pthread_join(p_group->ctrlLedThread, NULL))
      {
         printf("Can not join threads\n");
         exit(1);
      }
   }

   // Clean all Group and job database /free data...
   GroupInfoT* p_headGroup = p_allGroups;
   GroupInfoT* p_tempGroup = NULL;
   while (p_headGroup != NULL)
   {
      p_tempGroup = p_headGroup;
      p_headGroup = p_headGroup->p_nextGroup;
      free(p_tempGroup);
   }
   for (p_group = p_allGroups; p_group; p_group->p_nextGroup)
   {
      JobInfoT* p_job = NULL;
      JobInfoT* p_tempJob = NULL;
      for (p_job = p_group->p_allJobs; p_job; p_job = p_job->p_nextJob)
      {
         p_tempJob = p_job;
         free(p_job);   
      }
   }
	return 0;
}
