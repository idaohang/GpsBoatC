// **************************************************************
// GPS Boat
// main.c
// **************************************************************

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

// Wiring Pi library
#include <wiringPi.h>
#include <wiringSerial.h>

#include "includes.h"
#include "config.h" // defines I/O pins, operational parameters, etc.
#include "tools.h"
#include "TinyGPS++.h"
#include "HMC6343.h"
#include "Arduino.h"

#if USE_PI_PLATE
// LCD Library (local files)
#include "lcd.h"
#include "gpio.h"
#include "button.h"
#endif

//---------------------------------------------------------------
// external defines

//---------------------------------------------------------------
// local defines

#define LED_PIN		2
#define LED_ON		digitalWrite (LED_PIN, HIGH) ;	// On
#define LED_OFF		digitalWrite (LED_PIN, LOW) ;	// Off

//								|****************|
#define MSG_INIT				"Initializing    "
#define MSG_WAIT_FOR_GPS_LOCK	"Wait 4 GPS lock "
#define MSG_WAIT_FOR_GPS_STAB	"GPS Stabalize   "
#define MSG_WAIT_FOR_GPS_RELOCK "GPS Relocking   "
#define MSG_SET_NEXT_WAYPOINT	"Setting Waypoint"
#define MSG_START				"Starting Nav    "
#define MSG_RUN					"Running ...     "
#define MSG_STOP				"Stop Nav        "
#define MSG_IDLE				"Idle            "

#define LOOP_UPDATE_RATE_MS		200		// How often the main update loop runs

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
    double flat;
    double flon;
    double fmph;
    double fcourse;
    U8 hour;
    U8 minute;
    U8 second;
	bool bGpsLocked;
} tGPS_INFO;

#define GPS_DATA_KEY	0

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
TinyGPSPlus cGps;

// GPS info is volatile because it will be updated in its own thread
tGPS_INFO gtGpsInfo;
//volatile tGPS_INFO gtGpsInfo;

// Arduino on I2C bus
Arduino cArduino;

// Navigation Info
tNAV_INFO gtNavInfo;

#if USE_PI_PLATE
// Global button variable used by the THREAD_PiPlateButtons
Button gtActiveButton;
#endif

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

// Threads
PI_THREAD 	(THREAD_UpdateGps);
#if USE_PI_PLATE
PI_THREAD	(THREAD_PiPlateButtons);
#endif

// Normal local functions
void    	PrintProgramState( E_NAV_STATE eState );
#if USE_PI_PLATE
void		PrintProgramState_on_LCD( E_NAV_STATE eState );
Button		BTN_WaitForButton( Button ButtonToWaitFor );
Button		BTN_WaitForAnyButton( void );
Button		BTN_GetCurrentButton( void );
#endif
E_DIRECTION DirectionToBearing( float DestinationBearing, float CurrentBearing, float BearingTolerance );
void    	SetSpeed( int new_speed );
void		SetRudder( int new_setting );
float 		GetCompassHeading( float declination );

void		setup( void );
void		loop( void );

//---------------------------------------------------------------
// main
//---------------------------------------------------------------
int main(int argc, char **argv)
{
	E_NAV_STATE last_nav_state;
	int DisplayUpdateCounter = 0;

	system("clear");
	printf("GpsBoat - Version %s\n\n", SOFTWARE_VERSION);

	//-----------------------
	// Setup hardware
	//-----------------------
	printf("Setting up hardware:\n");
	setup();

	delay(3000);

#if USE_PI_PLATE
	LCD_colour( Blue );
	LCD_home();
	LCD_clear();

	LCD_printf("Press Select    ");
	BTN_WaitForButton( Select );
#endif

	last_nav_state = geNavState;
 
	//-----------------------
	// Main Loop
	//-----------------------
	printf("Starting Main Loop:\n");
	while(1)
	{
		loop();

	    delay( LOOP_UPDATE_RATE_MS );

		if( DisplayUpdateCounter-- <= 0 )
		{
			// reset display counter for 1000ms updates based on loop update rate
			DisplayUpdateCounter = 1000 / LOOP_UPDATE_RATE_MS;

			// Print system status
			system("clear");
			printf("Status: "); 
			PrintProgramState( geNavState ); printf("\n\n");

			printf("*** Navigation Info ***\n");
			printf("Bearing to Target: %i\n", gtNavInfo.bear_to_waypoint);
			printf("Distance to Target: %.1f meters\n", gtNavInfo.dist_to_waypoint);
			printf("Heading: %i\n", (U16)gtNavInfo.current_heading);
			printf("\n*** GPS Status ***\n");
			printf("GPS Locked: %s\n", (gtGpsInfo.bGpsLocked) ? "YES" : "NO");
			printf("GPS Lat: %f    Long: %f\n", gtGpsInfo.flat, gtGpsInfo.flon);
			printf("\n\n");

			// Only update the LCD when state changes
			if( last_nav_state != geNavState )
			{
#if USE_PI_PLATE
				PrintProgramState_on_LCD( geNavState );
				delay(2000);
#endif
				last_nav_state = geNavState;
			}

#if USE_PI_PLATE
			// Show distance to target
			LCD_cursor_goto(0, 0);
			LCD_printf("Dist: %3.1fm  ", gtNavInfo.dist_to_waypoint);
			// Show heading
			LCD_cursor_goto(1, 0);
			LCD_printf("Head: %3.1f", gtNavInfo.current_heading);
#endif
		}
	}

	return 0;
}

//-----------------------------------------------------------------------------------
void setup()
{
	char id_str[5];

    LED_ON;

	//-----------------------
	printf("WireingPi ... ");
	wiringPiSetup();
	printf("OK\n");

	//-----------------------
	printf("I/O Pins ... ");
	
    pinMode(LED_PIN, OUTPUT);

	printf("OK\n");

#if USE_PI_PLATE
	if( GPIO_open() < 0 )
	{
		exit(0);
	}
    
	LCD_init(0);

	LCD_home();
	LCD_clear();

	LCD_printf("GPS Boat");
	
	LCD_cursor_goto(1, 0);
	LCD_printf("Version %s", SOFTWARE_VERSION );

	LCD_colour( Red );

	// Start the button polling thread
	piThreadCreate( THREAD_PiPlateButtons );
#endif

#if USE_ARDUINO
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
#endif	// USE_ARDUINO

	//-----------------------
	printf("GPS ...\n");

	if ((gSerial_fd = serialOpen (GPS_SERIAL_PORT, GPS_BAUD)) < 0)
	{
		fprintf (stderr, "\tUnable to open serial device: %s\n", strerror (errno)) ;
		return;
	}
	else
	{
		printf("Comm port to GPS opened. GPS Baud: %i\n", GPS_BAUD);

		// Start the GPS thread
		piThreadCreate( THREAD_UpdateGps );
	}

	printf("OK\n");

	//-----------------------
	printf("Compass ... ");

	HMC6343_Setup();

	printf("OK\n");
     
    // Navigation state machine init
    geNavState = E_NAV_INIT;

    LED_OFF;
}

//-----------------------------------------------------------------------------------
void loop( void )
{
    // Set LED on
    LED_ON;
    
    // *******************************************
    // Update cGps Data/Status
	// This is now updated in the THREAD_UpdateGps
    // *******************************************
    
    // **********************************
    // Navigation State Machine variables
    // **********************************
    static float initial_dist_to_waypoint;
    float bearing_tolerance;
    static U8 update_counter = 0;  // 100ms x 10 == 1 second update since loop runs about every 100ms
    static S16 gps_delay = 0;
    
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
          gtNavInfo.bear_to_waypoint = cGps.courseTo( gtGpsInfo.flat, gtGpsInfo.flon,
          				    gtWayPoint[gTargetWP].flat, gtWayPoint[gTargetWP].flon );

		  gtNavInfo.dist_to_waypoint = cGps.distanceBetween( gtGpsInfo.flat, gtGpsInfo.flon,
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
              initial_dist_to_waypoint = cGps.distanceBetween( gtGpsInfo.flat, gtGpsInfo.flon,
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
              gtNavInfo.dist_to_waypoint = cGps.distanceBetween( gtGpsInfo.flat, gtGpsInfo.flon,
    						     gtWayPoint[gTargetWP].flat, gtWayPoint[gTargetWP].flon );
    
              gtNavInfo.bear_to_waypoint = cGps.courseTo( gtGpsInfo.flat, gtGpsInfo.flon,
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

//------------------------------------------------------------------------------
float GetCompassHeading( float declination )
{
	float heading = (float)(HMC6343_GetHeading()) / 10.0;

    // If you have an EAST declination, use + declinationAngle, if you
    // have a WEST declination, use - declinationAngle 

	heading -= declination;

    // Correct for when signs are reversed. 
    if( heading < 0.0 )
    {
		heading += 360.0;
    }

    // Check for wrap due to addition of declination. 
    if( heading > 360.0 )
    {
    	heading -= 360.0;
    }

	return heading;
}

//-----------------------------------------------------------------------------------
// Gradually sets the new ESC speed setting unless its STOP
// Assumes LOWER settings == faster
void SetSpeed( int new_setting )
{
  int last_setting;
  int step_and_dir;

#if USE_ARDUINO
	cArduino.SetReg( ARDUINO_REG_ESC, new_setting);
#endif  
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

#if USE_ARDUINO
	cArduino.SetReg( ARDUINO_REG_STEERING, new_setting);
#endif
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
PI_THREAD (THREAD_UpdateGps )
{
    static bool bLocked = false;
    bool bNewGpsData = false;
    unsigned long fix_age;
    int year;
    U8 month, day, hundredths;

	printf("THREAD_UpdateGps started\n");
	
	while( true )
	{
		delay( 200 );

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
			// Lock the GPS data for updating
			piLock( GPS_DATA_KEY );
        
		    // GPS Position
		    // retrieves +/- lat/long in 100000ths of a degree
			gtGpsInfo.flat = cGps.location.lat();
			gtGpsInfo.flon = cGps.location.lng();

		    if( cGps.location.isValid() )
		    {
		        gtGpsInfo.bGpsLocked = true;
		    }
		    else
		    {
		        gtGpsInfo.bGpsLocked = false;
		    }
		        
#if USE_GPS_TIME_INFO
		    // GPS Time
		    cGps.crack_datetime(&year, &month, &day, &gtGpsInfo.hour, &gtGpsInfo.minute, &gtGpsInfo.second, &hundredths, &fix_age);
#endif // USE_GPS_TIME_INFO

		    // GPS Speed
		    gtGpsInfo.fmph = cGps.speed.mph(); // speed in miles/hr
		    // course in 100ths of a degree
		    gtGpsInfo.fcourse = cGps.course.deg();

			// Unlock the GPS data for updating
			piUnlock( GPS_DATA_KEY );

			// reset new data flag			
			bNewGpsData = false;
		}
    }
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
float AngleCorrect(float inangle)
{
	float outangle = 0;

	outangle = inangle;

	while( outangle > 360)
	{
		outangle -= 360.0;
	}

	while (outangle < 0)
	{
		outangle += 360.0;
	}

	return(outangle);
}

//-----------------------------------------------------------------------------------
void PrintProgramState( E_NAV_STATE eState )
{
	switch( eState )
	{
	case E_NAV_INIT:
		printf(MSG_INIT);
		break;
	case E_NAV_WAIT_FOR_GPS_LOCK:
		printf(MSG_WAIT_FOR_GPS_LOCK);
		break;
	case E_NAV_WAIT_FOR_GPS_STABLIZE:
		printf(MSG_WAIT_FOR_GPS_STAB);
		break;
	case E_NAV_WAIT_FOR_GPS_RELOCK:
		printf(MSG_WAIT_FOR_GPS_RELOCK);
		break;
	case E_NAV_SET_NEXT_WAYPOINT:
		printf(MSG_SET_NEXT_WAYPOINT);
		break;
	case E_NAV_START:
		printf(MSG_START);
		break;
	case E_NAV_RUN:
		printf(MSG_RUN);
		break;
	case E_NAV_STOP:
		printf(MSG_STOP);
		break;
	case E_NAV_IDLE:
		printf(MSG_IDLE);
		break;
	}
}

//-----------------------------------------------------------------------------------
#if USE_PI_PLATE
void PrintProgramState_on_LCD( E_NAV_STATE eState )
{
	LCD_home();

	switch( eState )
	{
	case E_NAV_INIT:
		LCD_printf(MSG_INIT);
		break;
	case E_NAV_WAIT_FOR_GPS_LOCK:
		LCD_printf(MSG_WAIT_FOR_GPS_LOCK);
		break;
	case E_NAV_WAIT_FOR_GPS_STABLIZE:
		LCD_printf(MSG_WAIT_FOR_GPS_STAB);
		break;
	case E_NAV_WAIT_FOR_GPS_RELOCK:
		LCD_printf(MSG_WAIT_FOR_GPS_RELOCK);
		break;
	case E_NAV_SET_NEXT_WAYPOINT:
		LCD_printf(MSG_SET_NEXT_WAYPOINT);
		break;
	case E_NAV_START:
		LCD_printf(MSG_START);
		break;
	case E_NAV_RUN:
		LCD_printf(MSG_RUN);
		break;
	case E_NAV_STOP:
		LCD_printf(MSG_STOP);
		break;
	case E_NAV_IDLE:
		LCD_printf(MSG_IDLE);
		break;
	}
}

//-----------------------------------------------------------------------------------
PI_THREAD (THREAD_PiPlateButtons)
{
	Button butt;

	printf("THREAD_PiPlateButtons started\n");
	
	while( true )
	{
		delay( 200 );

		// Read button state
		butt = btn_nblk();

		if( butt != Null )
		{
			// A button is pressed. Wait for it to go back up
			printf("A button is pressed. Wait for it to go back up\n");
			while( btn_nblk() != Null )
			{
				delay(10);
			}

			// Button released. Set the global variable
			gtActiveButton = butt;
		}
	}
}
//-----------------------------------------------------------------------------------
Button BTN_WaitForButton( Button ButtonToWaitFor )
{
	while( ButtonToWaitFor != gtActiveButton )
	{
		delay(10);
	}
}

//-----------------------------------------------------------------------------------
Button BTN_WaitForAnyButton( void )
{
	gtActiveButton = Null;

	while( Null != gtActiveButton )
	{
		delay(10);
	}
}

//-----------------------------------------------------------------------------------
Button BTN_GetCurrentButton( void )
{
	return gtActiveButton;
}

#endif	// #if USE_PI_PLATE
