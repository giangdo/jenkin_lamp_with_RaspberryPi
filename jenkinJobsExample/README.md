* How to install jenkins server into your computer:
   + go to https://jenkins-ci.org/ and get install package.
   + after install, restart your computer
   + go to local host jenkins server:
     http://localhost:8080/
   + to know folder that is used to store all jenkins configuration:
      - goto http://localhost:8080/systemInfo
      - search JENKINS_HOME
      - default, this folder is /var/lib/jenkins
      
* Config some new jobs:
   + config manually
   + or we can copy this "jobs" folder into your JENKINS_HOME to have some configurated jobs in your jenkins server.
      - after copy we will have some jobs:
         http://localhost:8080/job/cphw_1/
         http://localhost:8080/job/cphw_2/
         http://localhost:8080/job/cphw_3/
         http://localhost:8080/job/pes_1/
         http://localhost:8080/job/pes_2/
         http://localhost:8080/job/plex_1/
