// **************************************************************
// GPS Boat
// main.c
// **************************************************************

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include "includes.h"
#include "config.h" // defines I/O pins, operational parameters, etc.
#include "TinyGPS.h"
#include "HMC58X3.h"
#include "ADXL345.h"
#include "Arduino.h"

//---------------------------------------------------------------
// local defines

#define LED_PIN		2
#define LED_ON		digitalWrite (LED_PIN, HIGH) ;	// On
#define LED_OFF		digitalWrite (LED_PIN, LOW) ;	// Off

typedef enum
{
  E_GO_LEFT,
  E_GO_RIGHT,
  E_GO_STRAIGHT
} E_DIRECTION;

// Program State Machine states
typedef enum
{
    E_NAV_INIT,
    E_NAV_WAIT_FOR_GPS_LOCK,    // progresses to E_NAV_SET_NEXT_WAYPOINT
    E_NAV_WAIT_FOR_GPS_STABLIZE,
    E_NAV_WAIT_FOR_GPS_RELOCK,  // resumes navigation state to E_NAV_START if GPS loses lock
    E_NAV_SET_NEXT_WAYPOINT,
    E_NAV_START,
    E_NAV_RUN,
    E_NAV_STOP,
    E_NAV_IDLE,
    
    E_NAV_MAX
} E_NAV_STATE;

typedef struct
{
    float flat;
    float flon;
    float fmph;
    float fcourse;
    U8 hour;
    U8 minute;
    U8 second;
	bool bGpsLocked;
} tGPS_INFO;

typedef struct
{
	float dist_to_waypoint;
	float bear_to_waypoint;
	float current_heading;
} tNAV_INFO;

typedef struct
{
    float flat;
    float flon;
} tWAY_POINT;

//---------------------------------------------------------------
// local data

// Global program state
E_NAV_STATE geNavState;

// GPS
int gSerial_fd;
TinyGPS cGps;
tGPS_INFO gtGpsInfo;

// Compass
HMC58X3 cCompass;

// Accelerometer
ADXL345 cAccel;

// Arduino on I2C bus
Arduino cArduino;

// Navigation Info
tNAV_INFO gtNavInfo;

int gTargetWP = 0;

// Way point table
tWAY_POINT gtWayPoint[] = {
#if USE_HOME_POSITION
    { WAYPOINT_HOME_LAT, WAYPOINT_HOME_LON },
#else
    { 0, 0 },                              // Home Waypoint (will set with inital lock)
#endif
    { WAYPOINT_A_LAT, WAYPOINT_A_LON },    // Waypoint A
    { WAYPOINT_B_LAT, WAYPOINT_B_LON },    // Waypoint B
    { WAYPOINT_C_LAT, WAYPOINT_C_LON },    // Waypoint C
    { WAYPOINT_D_LAT, WAYPOINT_D_LON },    // Waypoint D
    { WAYPOINT_E_LAT, WAYPOINT_E_LON },    // Waypoint E
    { WAYPOINT_F_LAT, WAYPOINT_F_LON },    // Waypoint F
    { WAYPOINT_G_LAT, WAYPOINT_G_LON },    // Waypoint G
    { WAYPOINT_H_LAT, WAYPOINT_H_LON },    // Waypoint H
    { WAYPOINT_I_LAT, WAYPOINT_I_LON },    // Waypoint I
    { WAYPOINT_J_LAT, WAYPOINT_J_LON },    // Waypoint J    
};


//---------------------------------------------------------------  
// local function prototypes

bool 	UpdateGps( tGPS_INFO *ptGpsInfo );
void    PrintProgramState( E_NAV_STATE eState );
E_DIRECTION DirectionToBearing( float DestinationBearing, float CurrentBearing, float BearingTolerance );
void    SetSpeed( int new_speed );
void    SetRudder( int new_setting );
float   GetCompassHeading( float declination );
void	setup( void );
void	loop( void );

//---------------------------------------------------------------
// main
//---------------------------------------------------------------
int main(int argc, char **argv)
{
	int x, y, z;

	system("clear");
	printf("GpsBoat - Version 1.0\n\n");

	//-----------------------
	// Setup hardware
	//-----------------------
	printf("Setting up hardware:\n");
	setup();

	delay(3000);
 
	//-----------------------
	// Main Loop
	//-----------------------
	printf("Starting Main Loop:\n");
	while(1)
	{
		loop();

		// Print system status
		system("clear");
		printf("Status: "); PrintProgramState( geNavState ); printf("\n");
		printf("Navigation Info:\n");
		printf("Bearing to Target: %i\n", gtNavInfo.bear_to_waypoint);
		printf("Distance to Target: %f meters\n", gtNavInfo.dist_to_waypoint);
		printf("Heading: %i\n", (U16)gtNavInfo.current_heading);
		printf("\n");
		printf("GPS Locked: %i\n", gtGpsInfo.bGpsLocked);
		printf("GPS Lat: %f    Long: %f\n", gtGpsInfo.flat, gtGpsInfo.flon);

		cAccel.readAccel(&x, &y, &z);
		printf("\nAccelerometer data: %i, %i, %i", x, y, z);

	    delay( 200 );
	}

	return 0;
}

//-----------------------------------------------------------------------------------
void setup()
{
	char id_str[5];

	//-----------------------
	printf("WireingPi ... ");
	wiringPiSetup();
	printf("OK\n");

	//-----------------------
	printf("I/O Pins ... ");
	
    pinMode(LED_PIN, OUTPUT);

	printf("OK\n");

	//-----------------------
	printf("Arduino ...\n");

	cArduino.Init( ARDUINO_I2C_ADDR );

	printf("\tArduino version: 0x%X\n", cArduino.GetReg( ARDUINO_REG_VERSION ) );

    // Init servos
	printf("Servo Test:\n");
	cArduino.SetReg( ARDUINO_REG_EXTRA_LED, 1 );

	printf("Left ... ");
    SetRudder( RUDDER_FULL_LEFT );
    delay(1000);
	printf("Center ... ");
    SetRudder( RUDDER_CENTER );
    delay(1000);
	printf("Right ... ");
    SetRudder( RUDDER_FULL_RIGHT );
    delay(1000);
	printf("Center ...\n");
    SetRudder( RUDDER_CENTER );
    delay(1000);

	cArduino.SetReg( ARDUINO_REG_EXTRA_LED, 0 );

	printf("OK\n");

	//-----------------------
	printf("GPS ...\n");

	if ((gSerial_fd = serialOpen ("/dev/ttyAMA0", GPS_BAUD)) < 0)
	{
		fprintf (stderr, "\tUnable to open serial device: %s\n", strerror (errno)) ;
		return;
	}
	else
	{
		printf("\tComm port to GPS opened. GPS Baud: %i\n", GPS_BAUD);
	}

	printf("OK\n");

	//-----------------------
	printf("Accelerometer ...\n");	

	cAccel.init( ADXL345_ADDR_ALT_LOW );

	printf("OK\n");

	//-----------------------
	printf("Compass ...\n");

    LED_ON;

    // Init compass class
    cCompass.init(false); // Dont set mode yet, we'll do that later on. 

	memset( id_str, 0, sizeof(id_str) );
	cCompass.getID( id_str );
	printf("Compass ID: %s\n", id_str );

    // Calibrate HMC using self test, not recommended to change the gain after calibration.
	printf("Calibrating ... ");
	{
		int i;
		for(i=1; i<2; i++)
		{
			printf("\nCaling with %i\n", i);
    		if(cCompass.calibrate(i, 32)) // Use gain 1=default, valid 0-7, 7 not recommended.
			{
				break;
			}
		}
	}

    // Single mode conversion was used in calibration, now set continuous mode
    cCompass.setMode(0);

    LED_OFF;

	printf("OK\n");
     
    // Navigation state machine init
    geNavState = E_NAV_INIT;
}

//-----------------------------------------------------------------------------------
void loop( void )
{
    // Set LED on
    LED_ON;
    
    // **********************
    // Update cGps Data/Status
    // **********************
    gtGpsInfo.bGpsLocked = UpdateGps( &gtGpsInfo );
    
    // **********************************
    // Navigation State Machine variables
    // **********************************
    static float initial_dist_to_waypoint;
    float bearing_tolerance;
    static U8 update_counter = 0;  // 100ms x 10 == 1 second update since loop runs about every 100ms
    static S16 gps_delay = 0;
	static E_NAV_STATE last_NavState = E_NAV_MAX;
    
	// **********************
	// Update compass heading
	// **********************
      
//      if( gtGpsInfo.fmph > 3.0 )
//      {
//        current_heading = gtGpsInfo.fcourse;
//      }
//      else
	{    
		gtNavInfo.current_heading = GetCompassHeading( MAG_VAR );
	}
      
	if( last_NavState != geNavState )
	{
		last_NavState = geNavState;
		PrintProgramState( geNavState );
	}

	// ******************
	// Main State Machine
	// ******************
      switch( geNavState )
      {
      case E_NAV_INIT:
          // Initialize wheels, motors, rudder, comms, etc.

          // Set initial way point target
          gTargetWP = 0;
          geNavState = E_NAV_WAIT_FOR_GPS_LOCK;
          break;
          
      case E_NAV_WAIT_FOR_GPS_LOCK:
          if( gtGpsInfo.bGpsLocked )
          {          
              gps_delay = GPS_STABALIZE_LOCK_TIME;
              geNavState = E_NAV_WAIT_FOR_GPS_STABLIZE;
          }
          break;
          
      case E_NAV_WAIT_FOR_GPS_STABLIZE:

          if( gps_delay-- )
          {
              delay(1000);
          }
          else
          {
#if !USE_HOME_POSITION
              // Save current GPS location as the "Home" waypoint
              gtWayPoint[0].flat = gtGpsInfo.flat;
              gtWayPoint[0].flon = gtGpsInfo.flon;
#endif
#if DO_GPS_TEST
              geNavState = E_NAV_IDLE;
#else
              geNavState = E_NAV_SET_NEXT_WAYPOINT;
#endif
          }
          break;
          
      case E_NAV_SET_NEXT_WAYPOINT:
          gTargetWP++;
          gTargetWP = gTargetWP % NUM_WAY_POINTS;
          
          // Calculate inital bearing to waypoint
          gtNavInfo.bear_to_waypoint = cGps.course_to( gtGpsInfo.flat, gtGpsInfo.flon,
          				    gtWayPoint[gTargetWP].flat, gtWayPoint[gTargetWP].flon );

		  gtNavInfo.dist_to_waypoint = cGps.distance_between( gtGpsInfo.flat, gtGpsInfo.flon,
   						    gtWayPoint[gTargetWP].flat, gtWayPoint[gTargetWP].flon );

          geNavState = E_NAV_START;
          break;
          
      case E_NAV_WAIT_FOR_GPS_RELOCK:
          // Resume navigation if the cGps obtains a lock again
          if( gtGpsInfo.bGpsLocked )
          {          
              geNavState = E_NAV_START;
          }
          break;
          
      case E_NAV_START:
          // Use motors, rudder and compass to turn towards new waypoint
          
          // Which way to turn?
          switch( DirectionToBearing( gtNavInfo.bear_to_waypoint, gtNavInfo.current_heading, DEGREES_TO_BEARING_TOLERANCE ) )
          {
            case E_GO_LEFT:
              printf("Go LEFT\n");
              SetRudder( RUDDER_FULL_LEFT );
              SetSpeed( SPEED_25_PERCENT );
              delay(100);
//              SetRudder( RUDDER_CENTER );
//              SetSpeed( SPEED_STOP );
              break;
            case E_GO_RIGHT:         
              printf("Go RIGHT\n");
              SetRudder( RUDDER_FULL_RIGHT );
              SetSpeed( SPEED_25_PERCENT );
              delay(100);
//              SetRudder( RUDDER_CENTER );
//              SetSpeed( SPEED_STOP );
              break;
            case E_GO_STRAIGHT:
              printf("Go STRAIGHT\n");
              geNavState = E_NAV_RUN;
              SetRudder( RUDDER_CENTER );
              SetSpeed( SPEED_50_PERCENT );

              // Calculate initial distance to next point
              initial_dist_to_waypoint = cGps.distance_between( gtGpsInfo.flat, gtGpsInfo.flon,
    						       gtWayPoint[gTargetWP].flat, gtWayPoint[gTargetWP].flon );
              gtNavInfo.dist_to_waypoint = initial_dist_to_waypoint;
              printf("Distance to waypoint: %f\n", gtNavInfo.dist_to_waypoint);
              break;
          }
          break;
          
      case E_NAV_RUN:
          if( 0 == (update_counter++ % 10) )
          {
              // Update range and bearing to waypoint
              gtNavInfo.dist_to_waypoint = cGps.distance_between( gtGpsInfo.flat, gtGpsInfo.flon,
    						     gtWayPoint[gTargetWP].flat, gtWayPoint[gTargetWP].flon );
    
              gtNavInfo.bear_to_waypoint = cGps.course_to( gtGpsInfo.flat, gtGpsInfo.flon,
              				     gtWayPoint[gTargetWP].flat, gtWayPoint[gTargetWP].flon );
          }
          
          // Is GPS still locked?
          if( false == gtGpsInfo.bGpsLocked )
          {            
              geNavState = E_NAV_STOP;
              break;   
          }
          
          // Adjust bearing to target tolerance for more refined direction pointing
          if( gtNavInfo.dist_to_waypoint <= (initial_dist_to_waypoint * 0.10) )
          {
            bearing_tolerance = DEGREES_TO_BEARING_TOLERANCE * 0.5;
          }
          else
          {
            bearing_tolerance = DEGREES_TO_BEARING_TOLERANCE;
          }
                   
          // Correct track to waypoint (if needed)
          switch( DirectionToBearing( gtNavInfo.bear_to_waypoint, gtNavInfo.current_heading, bearing_tolerance ) )
          {
            case E_GO_LEFT:           
              SetRudder( RUDDER_LEFT );
              break;
            case E_GO_RIGHT:             
              SetRudder( RUDDER_RIGHT );
              break;
            case E_GO_STRAIGHT:            
              SetRudder( RUDDER_CENTER );
              SetSpeed( SPEED_100_PERCENT );
              break;
          }
          
          // Are we there yet?
          if( gtNavInfo.dist_to_waypoint <= SWITCH_WAYPOINT_DISTANCE )
          {
              SetSpeed( SPEED_STOP );
              geNavState = E_NAV_SET_NEXT_WAYPOINT;
          }
          break;
          
      case E_NAV_STOP:
          // Stop navigation and wait to resume
          SetSpeed( SPEED_STOP );
          
          if( false == gtGpsInfo.bGpsLocked )
          {
              geNavState = E_NAV_WAIT_FOR_GPS_RELOCK;
              break;   
          }
          else
          {
              geNavState = E_NAV_IDLE;
          }
          break;
  
      case E_NAV_IDLE:
          // Wait for some external condition to restart us ... i.e. message from RPi, button push, etc.
          // TBD
          break;
      }
    
    // set the LED off
    LED_OFF;
}

//-----------------------------------------------------------------------------------
// Gradually sets the new ESC speed setting unless its STOP
// Assumes LOWER settings == faster
void SetSpeed( int new_setting )
{
  int last_setting;
  int step_and_dir;

	cArduino.SetReg( ARDUINO_REG_ESC, new_setting);
  
/*
  
  if( new_setting == SPEED_STOP )
  {
    gEscServo.write( SPEED_STOP );
    return;
  }
  
  // Get the last value written to the servo
  last_setting = gEscServo.read();
  
   // Which direction to go?
  step_and_dir = (new_setting > last_setting) ? SPEED_STEP_SIZE : SPEED_STEP_SIZE * -1;

  // Move to new setting gradually
  for( ; (step_and_dir > 0 ) ? (last_setting < new_setting) : (last_setting > new_setting); last_setting += step_and_dir )
  {
    if( last_setting > SPEED_BACKUP )  // don't let servo setting go negative!
    {
      break;
    }
    gEscServo.write(last_setting);
    delay(SPEED_STEP_DELAY);
  }
*/
}

//-----------------------------------------------------------------------------------
// Gradually sets the new rudder position
// Assums Right == Higher setting, Left == Lower setting
void SetRudder( int new_setting )
{
	int last_setting;
	int step_and_dir;
  
#if RUDDER_REVERSE
	new_setting = 180 - new_setting;
#endif

	cArduino.SetReg( ARDUINO_REG_STEERING, new_setting);
	return;
/*  
  // Get the last value written to the servo
  last_setting = gRudderServo.read();
  
  // Which direction to go?
  step_and_dir = (new_setting > last_setting) ? RUDDER_STEP_SIZE : RUDDER_STEP_SIZE * -1;

  // Move to new setting gradually
  for( ; (step_and_dir > 0 ) ? (last_setting < new_setting) : (last_setting > new_setting); last_setting += step_and_dir )
  {
    if( last_setting < 0 )  // don't let servo setting go negative!
    {
      break;
    }
    gRudderServo.write(last_setting);
    delay(RUDDER_STEP_DELAY);
  }
  
  // SoftSerial turns off interrupts and screws up the Servo lib! Need to detach!
//  gRudderServo.detach();
*/
}

//-----------------------------------------------------------------------------------
// Returns valid cGps data if GPS has a Fix
bool UpdateGps( tGPS_INFO *ptGpsInfo )
{
    static bool bLocked = false;
    bool bNewGpsData = false;
    unsigned long fix_age;
    int year;
    U8 month, day, hundredths;
	
    // *******************************    
    // Grab GPS data from serial input
    // *******************************
	while( serialDataAvail(gSerial_fd) )
    {
        char c;
		c = serialGetchar(gSerial_fd);

#if DO_GPS_TEST        
        printf("%c", c);
		fflush( stdout );
#endif
        if (cGps.encode(c))
        {
            bNewGpsData = true;
        }
    }
    
    // ********************
    // Process new cGps info
    // ********************
    if( bNewGpsData )
    {        
        // GPS Position
        // retrieves +/- lat/long in 100000ths of a degree
        cGps.f_get_position( &ptGpsInfo->flat, &ptGpsInfo->flon, &fix_age);
        if (fix_age == TinyGPS::GPS_INVALID_AGE)
        {
            bLocked = false;
        }
        else
        {
            bLocked = true;
        }
            
#if USE_GPS_TIME_INFO
        // GPS Time
        cGps.crack_datetime(&year, &month, &day, &ptGpsInfo->hour, &ptGpsInfo->minute, &ptGpsInfo->second, &hundredths, &fix_age);
#endif // USE_GPS_TIME_INFO

        // GPS Speed
        ptGpsInfo->fmph = cGps.f_speed_mph(); // speed in miles/hr
        // course in 100ths of a degree
        ptGpsInfo->fcourse = cGps.f_course();
    }
    
    return bLocked;
}

//-----------------------------------------------------------------------------------
E_DIRECTION DirectionToBearing( float DestinationBearing, float CurrentBearing, float BearingTolerance )
{
  E_DIRECTION eDirToGo;
  float Diff = DestinationBearing - CurrentBearing;
  float AbsDiff = abs(Diff);
  bool bNeg = (Diff < 0);
  bool bBig = (AbsDiff > 180.0);
  
  if( AbsDiff <= BearingTolerance )
  {
    // We're with-in a few degrees of the target. Just go straight!
    eDirToGo = E_GO_STRAIGHT;
  }
  else
  {
      if( !bNeg && !bBig )
          eDirToGo = E_GO_RIGHT;
      if( !bNeg && bBig )
          eDirToGo = E_GO_LEFT;
      if( bNeg && !bBig )
          eDirToGo = E_GO_LEFT;
      if( bNeg && bBig )
          eDirToGo = E_GO_RIGHT;
  }
  
  return eDirToGo;
}

//-----------------------------------------------------------------------------------
float GetCompassHeading( float declination )
{
    // Calculate heading when the magnetometer is level, then correctfor signs of axis. 
    float mag_x, mag_y, mag_z;
    
    cCompass.getValues( &mag_x, &mag_y, &mag_z );
    
    float heading = atan2( mag_y, mag_x );

    // If you have an EAST declination, use += declinationAngle, if you
    // have a WEST declination, use -= declinationAngle 

    heading -= radians(declination);

    // Correct for when signs are reversed. 
    if(heading < 0)
    {
	heading += TWO_PI;
    }

    // Check for wrap due to addition of declination. 
    if(heading > TWO_PI)
    {
    	heading -= TWO_PI;
    }

    // Convert radians to degrees for readability.
    return degrees( heading );
//    return (heading * 180/M_PI);
}

//-----------------------------------------------------------------------------------
void PrintProgramState( E_NAV_STATE eState )
{
	printf("State: ");
	switch( eState )
	{
	case E_NAV_INIT:
	  printf("Init");
	  break;
	case E_NAV_WAIT_FOR_GPS_LOCK:
	  printf("Wait for GPS Lock");
	  break;
	case E_NAV_WAIT_FOR_GPS_STABLIZE:
	  printf("Wait for GPS to Stabalize");
	  break;
	case E_NAV_WAIT_FOR_GPS_RELOCK:
	  printf("Wait for GPS Relock");
	  break;
	case E_NAV_SET_NEXT_WAYPOINT:
	  printf("Set Next Waypoint");
	  break;
	case E_NAV_START:
	  printf("Start");
	  break;
	case E_NAV_RUN:
	  printf("Run");
	  break;
	case E_NAV_STOP:
	  printf("Stop");
	  break;
	case E_NAV_IDLE:
	  printf("Idle");
	  break;
	}
	printf("\n");
	//fflush (stdout);
}

