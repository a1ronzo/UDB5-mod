// This file is part of MatrixPilot.
//
//    http://code.google.com/p/gentlenav/
//
// Copyright 2009-2011 MatrixPilot Team
// See the AUTHORS.TXT file for a list of authors of MatrixPilot.
//
// MatrixPilot is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// MatrixPilot is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with MatrixPilot.  If not, see <http://www.gnu.org/licenses/>.


#include "libDCM_internal.h"

//		These are the routines for maintaining a direction cosine matrix
//		that can be used to transform vectors between the earth and plane
//		coordinate systems. The 9 direction cosines in the matrix completely
//		define the orientation of the plane with respect to the earth.
//		The inverse of the matrix is equal to its transpose. This defines
//		the so-called orthogonality conditions, which impose 6 constraints on
//		the 9 elements of the matrix.

//	All numbers are stored in 2.14 format.
//	Vector and matrix libraries work in 1.15 format.
//	This combination allows values of matrix elements between -2 and +2.
//	Multiplication produces results scaled by 1/2.


#define RMAX15 0b0110000000000000	//	1.5 in 2.14 format

#define GGAIN SCALEGYRO*6*(RMAX*0.025)		//	integration multiplier for gyros at 40 Hz
//#define GGAIN SCALEGYRO*1.2*(RMAX*0.025)		//	integration multiplier for gyros at 200 Hz
fractional ggain[] =  { GGAIN , GGAIN , GGAIN } ;

unsigned int spin_rate = 0 ;
fractional spin_axis[] = { 0 , 0 , RMAX } ;

#if ( BOARD_TYPE == UDB3_BOARD || BOARD_TYPE == AUAV1_BOARD || BOARD_TYPE == UDB4_BOARD || BOARD_TYPE == UDB5_BOARD)
//Paul's gains corrected for GGAIN
#define KPROLLPITCH 256*5
#define KIROLLPITCH 256
#else
//Paul's gains:
#define KPROLLPITCH 256*10
#define KIROLLPITCH 256*2
#endif

#define KPYAW 256*4
#define KIYAW 32

#define GYROSAT 15000
// threshold at which gyros may be saturated

//	rmat is the matrix of direction cosines relating
//	the body and earth coordinate systems.
//	The columns of rmat are the axis vectors of the plane,
//	as measured in the earth reference frame.
//	rmat is initialized to the identity matrix in 2.14 fractional format

#ifdef INITIALIZE_VERTICAL  // for VTOL vertical initialization
fractional rmat[] = { RMAX , 0 , 0 , 0 , 0 , RMAX , 0 , -RMAX , 0 } ;
fractional rmatDelayCompensated[] =  { RMAX , 0 , 0 , 0 , 0 , RMAX , 0 , -RMAX , 0 } ;

#else // the usual case, horizontal initialization
fractional rmat[] = { RMAX , 0 , 0 , 0 , RMAX , 0 , 0 , 0 , RMAX } ;
fractional rmatDelayCompensated[] = { RMAX , 0 , 0 , 0 , RMAX , 0 , 0 , 0 , RMAX } ;
#endif

//	rup is the rotational update matrix.
//	At each time step, the new rmat is equal to the old one, multiplied by rup.
//  fractional rup[] = { RMAX , 0 , 0 , 0 , RMAX , 0 , 0 , 0 , RMAX } ;

//	gyro rotation vector:
fractional omegagyro[] = { 0 , 0 , 0 } ;
fractional omega[] = { 0 , 0 , 0 } ;

//	gyro correction vectors:
fractional omegacorrP[] = { 0 , 0 , 0 } ;
fractional omegacorrI[] = { 0 , 0 , 0 } ;

//  acceleration, as measured in GPS earth coordinate system
fractional accelEarth[] = { 0 , 0 , 0 } ;

//union longww accelEarthFiltered[] = { { 0 } , { 0 } ,  { 0 } } ;

//	correction vector integrators ;
union longww gyroCorrectionIntegral[] =  { { 0 } , { 0 } ,  { 0 } } ;

//	accumulator for computing adjusted omega:
fractional omegaAccum[] = { 0 , 0 , 0 } ;

//	gravity, as measured in plane coordinate system
#ifdef INITIALIZE_VERTICAL // VTOL vertical initialization
fractional gplane[] = { 0 , -GRAVITY , 0 } ;
#else  // horizontal initialization 
fractional gplane[] = { 0 , 0 , GRAVITY } ;
#endif

//	horizontal velocity over ground, as measured by GPS (Vz = 0 )
fractional dirovergndHGPS[] = { 0 , RMAX , 0 } ;

//	horizontal direction over ground, as indicated by Rmatrix
fractional dirovergndHRmat[] = { 0 , RMAX , 0 } ;

//	rotation angle equal to omega times integration factor:
//  fractional theta[] = { 0 , 0 , 0 } ;

//	matrix buffer:
//  fractional rbuff[] = { 0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 } ;

//	vector buffer
fractional errorRP[] = { 0 , 0 , 0 } ;
fractional errorYawground[] = { 0 , 0 , 0 } ;
fractional errorYawplane[]  = { 0 , 0 , 0 } ;

//	measure of error in orthogonality, used for debugging purposes:
fractional error = 0 ;

#if(MAG_YAW_DRIFT == 1)
fractional declinationVector[2] ;
#endif

#if(DECLINATIONANGLE_VARIABLE == 1)
union intbb dcm_declination_angle;
#endif

void dcm_init_rmat( void )
{
#if(MAG_YAW_DRIFT == 1)
 #if (DECLINATIONANGLE_VARIABLE == 1)
	dcm_declination_angle.BB = DECLINATIONANGLE;
 #endif
	declinationVector[0] = cosine( (signed char) (DECLINATIONANGLE >> 8) ) ;
	declinationVector[1] = sine( (signed char) (DECLINATIONANGLE >> 8) ) ;
#endif
}

//	Implement the cross product. *dest = *src1X*src2 ;
void VectorCross( fractional * dest , fractional * src1 , fractional * src2 )
{
	union longww crossaccum ;
	crossaccum.WW = __builtin_mulss( src1[1] , src2[2] ) ;
	crossaccum.WW -= __builtin_mulss( src1[2] , src2[1] ) ;
	crossaccum.WW *= 4 ;
	dest[0] = crossaccum._.W1 ;
	crossaccum.WW = __builtin_mulss( src1[2] , src2[0] ) ;
	crossaccum.WW -= __builtin_mulss( src1[0] , src2[2] ) ;
	crossaccum.WW *= 4 ;
	dest[1] = crossaccum._.W1 ;
	crossaccum.WW = __builtin_mulss( src1[0] , src2[1] ) ;
	crossaccum.WW -= __builtin_mulss( src1[1] , src2[0] ) ;
	crossaccum.WW *= 4 ;
	dest[2] = crossaccum._.W1 ;
	return ;
}


void read_gyros()
//	fetch the gyro signals and subtract the baseline offset, 
//	and adjust for variations in supply voltage
{
	unsigned spin_rate_over_2 ;
#if ( HILSIM == 1 )
	omegagyro[0] = q_sim.BB;
	omegagyro[1] = p_sim.BB;
	omegagyro[2] = r_sim.BB;  
#else
	omegagyro[0] = XRATE_VALUE ;
	omegagyro[1] = YRATE_VALUE ;
	omegagyro[2] = ZRATE_VALUE ;
#endif

	spin_rate = vector3_mag( omegagyro[0] , omegagyro[1] , omegagyro[2] ) ;
	spin_rate_over_2 = spin_rate >> 1 ;

	if ( spin_rate_over_2 > 0 )
	{
		spin_axis[0] = __builtin_divsd( ((long)omegagyro[0]) << 13 , spin_rate_over_2 ) ;
		spin_axis[1] = __builtin_divsd( ((long)omegagyro[1]) << 13 , spin_rate_over_2 ) ;
		spin_axis[2] = __builtin_divsd( ((long)omegagyro[2]) << 13 , spin_rate_over_2 ) ;
	}

	return ;
}

void read_accel()
{
#if ( HILSIM == 1 )
	gplane[0] = v_dot_sim.BB;
	gplane[1] = u_dot_sim.BB; 
	gplane[2] = w_dot_sim.BB;
#else
	gplane[0] =   XACCEL_VALUE ;
	gplane[1] =   YACCEL_VALUE ;
	gplane[2] =   ZACCEL_VALUE ;
#endif
	
	accelEarth[0] =  VectorDotProduct( 3 , &rmat[0] , gplane )<<1;
	accelEarth[1] = - VectorDotProduct( 3 , &rmat[3] , gplane )<<1;
	accelEarth[2] = -((int)GRAVITY) + (VectorDotProduct( 3 , &rmat[6] , gplane )<<1);

//	accelEarthFiltered[0].WW += ((((long)accelEarth[0])<<16) - accelEarthFiltered[0].WW)>>5 ;
//	accelEarthFiltered[1].WW += ((((long)accelEarth[1])<<16) - accelEarthFiltered[1].WW)>>5 ;
//	accelEarthFiltered[2].WW += ((((long)accelEarth[2])<<16) - accelEarthFiltered[2].WW)>>5 ;
	
	return ;
}

//	multiplies omega times speed, and scales appropriately
//  omega in radians per second, speed in cm per second
int omegaSOG ( int omega , unsigned int speed  )
{
	union longww working ;
	speed = speed>>3 ;
	working.WW = __builtin_mulsu( omega , speed ) ;
	if ( ((int)working._.W1 )> ((int)CENTRIFSAT) )
	{
		return RMAX ;
	}
	else if ( ((int)working._.W1) < ((int)-CENTRIFSAT) )
	{
		return - RMAX ;
	}
	else
	{
		working.WW = working.WW>>5 ;
		working.WW = __builtin_mulsu( working._.W0 , CENTRISCALE ) ;
		working.WW = working.WW<<5 ;
		return working._.W1 ;
	}
}

//	Lets leave it like this for a while, just in case we need to revert roll_pitch_drift.

void adj_accel()
{
	// total (3D) airspeed in cm/sec is used to adjust for acceleration
	gplane[0]=gplane[0]- omegaSOG( omegaAccum[2] , air_speed_3DGPS ) ;
	gplane[2]=gplane[2]+ omegaSOG( omegaAccum[0] , air_speed_3DGPS ) ;
	gplane[1]=gplane[1]+ ((unsigned int)(ACCELSCALE))*forward_acceleration ;
	
	return ;
}


//	The update algorithm!!
void rupdate(void)
//	This is the key routine. It performs a small rotation
//	on the direction cosine matrix, based on the gyro vector and correction.
//	It uses vector and matrix routines furnished by Microchip.
{
	fractional rup[9] ;
	fractional theta[3] ;
	fractional rbuff[9] ;
	unsigned long thetaSquare ;
	unsigned nonlinearAdjust ;
	
	VectorAdd( 3 , omegaAccum , omegagyro , omegacorrI ) ;
	VectorAdd( 3 , omega , omegaAccum , omegacorrP ) ;
	//	scale by the integration factors:
	VectorMultiply( 3 , theta , omega , ggain ) ; // Scalegain of 2 
	// diagonal elements of the update matrix:
	rup[0] = rup[4] = rup[8]= RMAX ;

	// compute the square of rotation

	thetaSquare = 	__builtin_mulss ( theta[0] , theta[0] ) +
					__builtin_mulss ( theta[1] , theta[1] ) +
					__builtin_mulss ( theta[2] , theta[2] ) ;

	// adjust gain by rotation_squared divided by 3

	nonlinearAdjust = RMAX + ((unsigned int ) ( thetaSquare >>14 ))/3 ;	

	theta[0] = __builtin_mulsu ( theta[0] , nonlinearAdjust )>>14 ;
	theta[1] = __builtin_mulsu ( theta[1] , nonlinearAdjust )>>14 ;
	theta[2] = __builtin_mulsu ( theta[2] , nonlinearAdjust )>>14 ;

	//	construct the off-diagonal elements of the update matrix:
	rup[1] = -theta[2] ;
	rup[2] =  theta[1] ;
	rup[3] =  theta[2] ;
	rup[5] = -theta[0] ;
	rup[6] = -theta[1] ;
	rup[7] =  theta[0] ;

	//	matrix multiply the rmatrix by the update matrix
	MatrixMultiply( 3 , 3 , 3 , rbuff , rmat , rup ) ;
	//	multiply by 2 and copy back from rbuff to rmat:
	MatrixAdd( 3 , 3 , rmat , rbuff , rbuff ) ; 
	return ;
}

//	normalization algorithm:
void normalize(void)
//	This is the routine that maintains the orthogonality of the
//	direction cosine matrix, which is expressed by the identity
//	relationship that the cosine matrix multiplied by its
//	transpose should equal the identity matrix.
//	Small adjustments are made at each time step to assure orthogonality.
{
	fractional norm ; // actual magnitude
	fractional renorm ;	// renormalization factor
	fractional rbuff[9] ;
	//	compute -1/2 of the dot product between rows 1 and 2
	error =  - VectorDotProduct( 3 , &rmat[0] , &rmat[3] ) ; // note, 1/2 is built into 2.14
	//	scale rows 1 and 2 by the error
	VectorScale( 3 , &rbuff[0] , &rmat[3] , error ) ;
	VectorScale( 3 , &rbuff[3] , &rmat[0] , error ) ;
	//	update the first 2 rows to make them closer to orthogonal:
	VectorAdd( 3 , &rbuff[0] , &rbuff[0] , &rmat[0] ) ;
	VectorAdd( 3 , &rbuff[3] , &rbuff[3] , &rmat[3] ) ;
	//	use the cross product of the first 2 rows to get the 3rd row
	VectorCross( &rbuff[6] , &rbuff[0] , &rbuff[3] ) ;


	//	Use a Taylor's expansion for 1/sqrt(X*X) to avoid division in the renormalization
	//	rescale row1
	norm = VectorPower( 3 , &rbuff[0] ) ; // Scalegain of 0.5
	renorm = RMAX15 - norm ;
	VectorScale( 3 , &rbuff[0] , &rbuff[0] , renorm ) ;
	VectorAdd( 3 , &rmat[0] , &rbuff[0] , &rbuff[0] ) ;
	//	rescale row2
	norm = VectorPower( 3 , &rbuff[3] ) ;
	renorm = RMAX15 - norm ;
	VectorScale( 3 , &rbuff[3] , &rbuff[3] , renorm ) ;
	VectorAdd( 3 , &rmat[3] , &rbuff[3] , &rbuff[3] ) ;
	//	rescale row3
	norm = VectorPower( 3 , &rbuff[6] ) ;
	renorm = RMAX15 - norm ;
	VectorScale( 3 , &rbuff[6] , &rbuff[6] , renorm ) ;
	VectorAdd( 3 , &rmat[6] , &rbuff[6] , &rbuff[6] ) ;
	return ;
}

//	Lets leave this for a while in case we need to revert roll_pitch_drift

#ifndef NEW_ACCELERATION_COMPENSATION
void roll_pitch_drift()
{
	VectorCross( errorRP , gplane , &rmat[6] ) ;
	return ;
}
#endif

long int accelerometer_earth_integral[3] = { 0 , 0 , 0 } ;
int GPS_velocity_previous[3] = { 0 , 0 , 0 } ;
unsigned int accelerometer_samples = 0 ;
#define MAX_ACCEL_SAMPLES 45
#define ACCEL_SAMPLES_PER_SEC 40

#ifdef NEW_ACCELERATION_COMPENSATION
void roll_pitch_drift()
{

	int accelerometer_earth[3] ;
	int GPS_acceleration[3] ;
	int accelerometer_reference[3] ;
	int errorRP_earth[3] ;

//	integrate the accelerometer signals in earth frame of reference

	accelerometer_earth_integral[0] +=  (VectorDotProduct( 3 , &rmat[0] , gplane )<<1);
	accelerometer_earth_integral[1] +=  (VectorDotProduct( 3 , &rmat[3] , gplane )<<1);
	accelerometer_earth_integral[2] +=  (VectorDotProduct( 3 , &rmat[6] , gplane )<<1);
	accelerometer_samples++;

//	if there is GPS information available, or if GPS is offline and there are enough samples, compute the roll-pitch correction
	if ( ( dcm_flags._.rollpitch_req == 1 ) || ( accelerometer_samples > MAX_ACCEL_SAMPLES ) )
	{
		if ( accelerometer_samples > 0 )
		{
			// compute the average of the integral
			accelerometer_earth[0] = __builtin_divsd( accelerometer_earth_integral[0] , accelerometer_samples ) ;
			accelerometer_earth[1] = __builtin_divsd( accelerometer_earth_integral[1] , accelerometer_samples ) ;
			accelerometer_earth[2] = __builtin_divsd( accelerometer_earth_integral[2] , accelerometer_samples ) ;
		}
		else
		{
			accelerometer_earth[0] = accelerometer_earth[1] = 0 ;
			accelerometer_earth[2] = GRAVITY ;
		}

		if ( (HILSIM==1) || (!gps_nav_valid() ))
		{
			// cannot do acceleration compensation, assume no acceleration 
			accelerometer_reference[0] = accelerometer_reference[1] = 0 ;
			accelerometer_reference[2] = RMAX ;
		}
		else
		{
			if ( dcm_flags._.rollpitch_req == 1 )
			{
				GPS_acceleration[0] = __builtin_divsd( __builtin_mulsu(( GPSvelocity.x - GPS_velocity_previous[0] ) , ACCEL_SAMPLES_PER_SEC ) , accelerometer_samples ) ;
				GPS_acceleration[1] = __builtin_divsd( __builtin_mulsu(( GPSvelocity.y - GPS_velocity_previous[1] ) , ACCEL_SAMPLES_PER_SEC ) , accelerometer_samples ) ;
				GPS_acceleration[2] = __builtin_divsd( __builtin_mulsu(( GPSvelocity.z - GPS_velocity_previous[2] ) , ACCEL_SAMPLES_PER_SEC ) , accelerometer_samples ) ;

				GPS_velocity_previous[0] = GPSvelocity.x ;
				GPS_velocity_previous[1] = GPSvelocity.y ;
				GPS_velocity_previous[2] = GPSvelocity.z ;
			}
			else
			{
				GPS_acceleration[0] = GPS_acceleration[1] = GPS_acceleration[2] = 0 ;
			}

			// acceleration_reference = normalize ( gravity_earth - 40* delta_GPS_velocity/samples )

			accelerometer_reference[0] = GPS_acceleration[0] ; // GPS x is opposite sign to DCM x
			accelerometer_reference[1] = - GPS_acceleration[1] ; // GPS y is same sign as DCM y
			accelerometer_reference[2] = 981 + GPS_acceleration[2] ; // gravity is 981 centimeters/sec/sec, z sign is opposite
			
			vector3_normalize( accelerometer_reference , accelerometer_reference ) ;
		}

		//	error_earth = accelerometer_earth cross accelerometer_reference, set error_earth[2] = 0 ;
		VectorCross( errorRP_earth , accelerometer_earth , accelerometer_reference ) ;
		errorRP_earth[2] = 0 ;
		
		//	error_body = Rtranspose * error_earth

		//	*** Note: this accomplishes multiplication rmat transpose times errorRP_earth!!
		MatrixMultiply( 1 , 3 , 3 , errorRP , errorRP_earth , rmat ) ;
		accelerometer_earth_integral[0] = accelerometer_earth_integral[1] = accelerometer_earth_integral[2] = 0 ;
		accelerometer_samples = 0 ;
		dcm_flags._.rollpitch_req = 0 ;
	}	
	return ;
}
#endif

void yaw_drift()
{
	//	although yaw correction is done in horizontal plane,
	//	this is done in 3 dimensions, just in case we change our minds later
	//	form the horizontal direction over ground based on rmat
	if (dcm_flags._.yaw_req )
	{
		if ( ground_velocity_magnitudeXY > GPS_SPEED_MIN )
		{
			//	vector cross product to get the rotation error in ground frame
			VectorCross( errorYawground , dirovergndHRmat , dirovergndHGPS ) ;
			//	convert to plane frame:
			//	*** Note: this accomplishes multiplication rmat transpose times errorYawground!!
			MatrixMultiply( 1 , 3 , 3 , errorYawplane , errorYawground , rmat ) ;
		}
		else
		{
			errorYawplane[0] = errorYawplane[1] = errorYawplane[2] = 0 ;
		}
		
		dcm_flags._.yaw_req = 0 ;
	}
	return ;
}


#if (MAG_YAW_DRIFT == 1)

fractional magFieldEarth[3] ;
extern fractional udb_magFieldBody[3] ;
extern fractional udb_magOffset[3] ;
fractional rmatPrevious[9] ;
fractional magFieldEarthNormalizedPrevious[3] ;
fractional magAlignment[4] = { 0 , 0 , 0 , RMAX } ;
fractional magFieldBodyMagnitudePrevious ;
fractional magFieldBodyPrevious[3] ;

#ifdef INITIALIZE_VERTICAL // vertical initialization for VTOL
void align_rmat_to_mag(void)
{
	unsigned char theta ;
	struct relative2D initialBodyField ;
	int costheta ;
	int sintheta ;
	initialBodyField.x = udb_magFieldBody[0] ;
	initialBodyField.y = udb_magFieldBody[2] ;
#if(DECLINATIONANGLE_VARIABLE == 1)
	theta = rect_to_polar( &initialBodyField ) -64 - (dcm_declination_angle._.B1) ;	
#else
	theta = rect_to_polar( &initialBodyField ) -64 - (DECLINATIONANGLE >> 8) ;
#endif
	costheta = cosine(theta) ;
	sintheta = sine(theta) ;
	rmat[0] = rmat[5] = costheta ;
	rmat[2] = sintheta ;
	rmat[3] = - sintheta ;
	return ;
}

#else // horizontal initialization for usual cases
void align_rmat_to_mag(void)
{
	unsigned char theta ;
	struct relative2D initialBodyField ;
	int costheta ;
	int sintheta ;
	initialBodyField.x = udb_magFieldBody[0] ;
	initialBodyField.y = udb_magFieldBody[1] ;
#if(DECLINATIONANGLE_VARIABLE == 1)
	theta = rect_to_polar( &initialBodyField ) -64 - (dcm_declination_angle._.B1) ;
#else
	theta = rect_to_polar( &initialBodyField ) -64 - (DECLINATIONANGLE >> 8) ;
#endif
	costheta = cosine(theta) ;
	sintheta = sine(theta) ;
	rmat[0] = rmat[4] = costheta ;
	rmat[1] = sintheta ;
	rmat[3] = - sintheta ;
	return ;
}
#endif

void quaternion_adjust( fractional quaternion[] , fractional direction[] )
{
//	performs an adjustment to a quaternion representation of re-alignement.
//	the cross product is left out, theory and test both show it should not be used.
	fractional delta_cos ;
	fractional vector_buffer[3] ;
	fractional increment[3] ;
	unsigned long int magnitudesqr ;
	unsigned magnitude ;
	increment[0] = direction[0]>>3 ;
	increment[1] = direction[1]>>3 ;
	increment[2] = direction[2]>>3 ;
	// change is cosine is 1/2 of the dot product of first 3 elements of quaternion
	// with the increment. The 1/2 is built into the dot product.
	delta_cos = - VectorDotProduct( 3 , quaternion , increment ) ;
	// the change in the first 3 elements is 1/2 of the 4 element times the increment.
	// There is a 1/2 built into the VectorScale 
	VectorScale( 3 , vector_buffer , increment , quaternion[3] ) ; 
	// Update the first three components
	VectorAdd( 3 , quaternion , quaternion , vector_buffer ) ;
	// Update the 4th component
	quaternion[3] += delta_cos ;
	// Renormalize
	magnitudesqr = __builtin_mulss( quaternion[0] , quaternion[0] )
			+ __builtin_mulss( quaternion[1] , quaternion[1] )
			+ __builtin_mulss( quaternion[2] , quaternion[2] )
			+ __builtin_mulss( quaternion[3] , quaternion[3] ) ;
	magnitude = sqrt_long( magnitudesqr ) ;

	quaternion[0] = __builtin_divsd( __builtin_mulsu ( quaternion[0] , RMAX ) , magnitude ) ;
	quaternion[1] = __builtin_divsd( __builtin_mulsu ( quaternion[1] , RMAX ) , magnitude ) ;
	quaternion[2] = __builtin_divsd( __builtin_mulsu ( quaternion[2] , RMAX ) , magnitude ) ;
	quaternion[3] = __builtin_divsd( __builtin_mulsu ( quaternion[3] , RMAX ) , magnitude ) ;

	return ;
}

void RotVector2RotMat( fractional rotation_matrix[] , fractional rotation_vector[] )
{
//	rotation vector represents a rotation in vector form
//	around an axis equal to the normalized value of the vector.
//	It is assumed that rotation_vector already includes a factor of sin(alpha/2)
//  maximum rotation is plus minus 180 degrees.
	fractional cos_alpha ;
	fractional cos_half_alpha ;
	fractional cos_half_alpha_rotation_vector[3] ;
	union longww sin_half_alpha_sqr = { 0 } ;
	int matrix_index ;

	cos_half_alpha = rotation_vector[3] ;

//	compute the square of sine of half alpha
	for ( matrix_index = 0 ; matrix_index < 3 ; matrix_index++ )
	{
		sin_half_alpha_sqr.WW += __builtin_mulss( rotation_vector[matrix_index] , rotation_vector[matrix_index] );
	}
	if ( sin_half_alpha_sqr.WW > ( (long) RMAX*RMAX - 1))
	{
		sin_half_alpha_sqr.WW = (long) RMAX*RMAX - 1 ;
	}

//	compute cos_alpha
	sin_half_alpha_sqr.WW *= 8 ;
	cos_alpha = RMAX - sin_half_alpha_sqr._.W1 ;

//	scale rotation_vector by 2*cos_half_alpha
	VectorScale ( 3 , cos_half_alpha_rotation_vector ,  rotation_vector , cos_half_alpha ) ;
	for ( matrix_index = 0 ; matrix_index < 3 ; matrix_index++ )
	{
		cos_half_alpha_rotation_vector[matrix_index] *= 4 ;
	}

//	compute 2 times rotation_vector times its transpose
	MatrixMultiply( 3 , 1 , 3 , rotation_matrix , rotation_vector , rotation_vector ) ;
	for ( matrix_index = 0 ; matrix_index < 9 ; matrix_index++ )
	{
		rotation_matrix[matrix_index] *= 4 ;
	}

	rotation_matrix[0] += cos_alpha ;
	rotation_matrix[4] += cos_alpha ;
	rotation_matrix[8] += cos_alpha ;

	rotation_matrix[1] -= cos_half_alpha_rotation_vector[2] ;
	rotation_matrix[2] += cos_half_alpha_rotation_vector[1] ;
	rotation_matrix[3] += cos_half_alpha_rotation_vector[2] ;
	rotation_matrix[5] -= cos_half_alpha_rotation_vector[0] ;
	rotation_matrix[6] -= cos_half_alpha_rotation_vector[1] ;
	rotation_matrix[7] += cos_half_alpha_rotation_vector[0] ;

	return ;
}

#define MAG_LATENCY 0.085 // seconds
#define MAG_LATENCY_COUNT ( ( int ) ( MAG_LATENCY / 0.025 ) )

int mag_latency_counter = 10 - MAG_LATENCY_COUNT ;

void mag_drift()
{
	int mag_error ;
	fractional magFieldEarthNormalized[3];
	fractional magFieldEarthHorzNorm[2] ;
	fractional magAlignmentError[3] ;
	fractional rmat2Transpose[9] ;
	fractional R2TR1RotationVector[3] ;
	fractional R2TAlignmentErrorR1[3] ;
	fractional rmatBufferA[9] ;
	fractional rmatBufferB[9] ;
	fractional magAlignmentAdjustment[3] ;
	fractional vectorBuffer[3] ;
	fractional magFieldBodyMagnitude ;
	fractional offsetEstimate[3] ;

	// the following compensates for magnetometer drift by adjusting the timing
	// of when rmat is read
	mag_latency_counter -- ;
	if ( mag_latency_counter == 0 )
	{
		VectorCopy ( 9 , rmatDelayCompensated , rmat ) ;
		mag_latency_counter = 10 ; // not really needed, but its good insurance
	}
	
	if ( dcm_flags._.mag_drift_req )
	{

//		Compute magnetic offsets
		magFieldBodyMagnitude =	vector3_mag( udb_magFieldBody[0], udb_magFieldBody[1], udb_magFieldBody[2] ) ;
		VectorSubtract( 3, vectorBuffer , udb_magFieldBody , magFieldBodyPrevious ) ;
		vector3_normalize( vectorBuffer , vectorBuffer ) ;
		VectorScale( 3 , offsetEstimate , vectorBuffer , magFieldBodyMagnitude - magFieldBodyMagnitudePrevious ) ;
		VectorCopy ( 3 , magFieldBodyPrevious , udb_magFieldBody ) ;
		magFieldBodyMagnitudePrevious = magFieldBodyMagnitude ;

//		Compute and apply the magnetometer alignment adjustment in the body frame
		RotVector2RotMat( rmatBufferA , magAlignment ) ;
		vectorBuffer[0] = VectorDotProduct( 3 , &rmatBufferA[0] , udb_magFieldBody ) << 1 ; 
		vectorBuffer[1] = VectorDotProduct( 3 , &rmatBufferA[3] , udb_magFieldBody ) << 1 ; 
		vectorBuffer[2] = VectorDotProduct( 3 , &rmatBufferA[6] , udb_magFieldBody ) << 1 ; 
		VectorCopy( 3 , udb_magFieldBody , vectorBuffer ) ;

		if ( dcm_flags._.first_mag_reading == 1 )
		{
			align_rmat_to_mag() ;
			VectorCopy ( 9 , rmatDelayCompensated , rmat ) ;		
		}

		mag_latency_counter = 10 - MAG_LATENCY_COUNT ; // setup for the next reading

//		Compute the mag field in the earth frame

		magFieldEarth[0] = VectorDotProduct( 3 , &rmatDelayCompensated[0] , udb_magFieldBody )<<1 ;
		magFieldEarth[1] = VectorDotProduct( 3 , &rmatDelayCompensated[3] , udb_magFieldBody )<<1 ;
		magFieldEarth[2] = VectorDotProduct( 3 , &rmatDelayCompensated[6] , udb_magFieldBody )<<1 ;

//		Normalize the magnetic vector to RMAT

		vector3_normalize ( magFieldEarthNormalized , magFieldEarth ) ;
		vector2_normalize ( magFieldEarthHorzNorm , magFieldEarth ) ;

//		Use the magnetometer to detect yaw drift

#if(DECLINATIONANGLE_VARIABLE == 1)
		declinationVector[0] = cosine(dcm_declination_angle._.B1) ;
		declinationVector[1] = sine(dcm_declination_angle._.B1) ;
#endif		
		mag_error = VectorDotProduct( 2 , magFieldEarthHorzNorm , declinationVector ) ;
		VectorScale( 3 , errorYawplane , &rmat[6] , mag_error ) ; // Scalegain = 1/2

//		Do the computations needed to compensate for magnetometer misalignment

//		Determine the apparent shift in the earth's magnetic field:
		VectorCross( magAlignmentError, magFieldEarthNormalizedPrevious , magFieldEarthNormalized ) ;

//		Compute R2 transpose
		MatrixTranspose( 3 , 3 , rmat2Transpose , rmatDelayCompensated ) ;

//		Compute 1/2 of R2tranpose times R1
		MatrixMultiply( 3 , 3 , 3 , rmatBufferA , rmat2Transpose , rmatPrevious ) ;

//		Convert to a rotation vector, take advantage of 1/2 from the previous step
		R2TR1RotationVector[0] = rmatBufferA[7] - rmatBufferA[5] ;
		R2TR1RotationVector[1] = rmatBufferA[2] - rmatBufferA[6] ;
		R2TR1RotationVector[2] = rmatBufferA[3] - rmatBufferA[1] ;

//		Compute 1/4 of RT2*Matrix(error-vector)*R1
		rmatBufferA[0] = rmatBufferA[4] = rmatBufferA[8]=0 ;
		rmatBufferA[7] =  magAlignmentError[0] ;
		rmatBufferA[5] = -magAlignmentError[0] ;
		rmatBufferA[2] =  magAlignmentError[1] ;
		rmatBufferA[6] = -magAlignmentError[1] ;
		rmatBufferA[3] =  magAlignmentError[2] ;
		rmatBufferA[1] = -magAlignmentError[2] ;
		MatrixMultiply( 3 , 3 , 3 , rmatBufferB , rmatBufferA , rmatDelayCompensated ) ;
		MatrixMultiply( 3 , 3 , 3 , rmatBufferA , rmat2Transpose , rmatBufferB ) ;

//		taking advantage of factor of 1/4 in the two matrix multiplies, compute
//		1/2 of the vector representation of the rotation
		R2TAlignmentErrorR1[0] = ( rmatBufferA[7] - rmatBufferA[5] ) ;
		R2TAlignmentErrorR1[1] = ( rmatBufferA[2] - rmatBufferA[6] ) ;
		R2TAlignmentErrorR1[2] = ( rmatBufferA[3] - rmatBufferA[1] ) ;

//		compute the negative of estimate of the residual misalignment
		VectorCross( magAlignmentAdjustment  , R2TAlignmentErrorR1 , R2TR1RotationVector ) ;
		
		if ( dcm_flags._.first_mag_reading == 0 )
		{

			udb_magOffset[0] = udb_magOffset[0] + ( ( offsetEstimate[0] + 2 ) >> 2 ) ;
			udb_magOffset[1] = udb_magOffset[1] + ( ( offsetEstimate[1] + 2 ) >> 2 ) ;
			udb_magOffset[2] = udb_magOffset[2] + ( ( offsetEstimate[2] + 2 ) >> 2 ) ;

			quaternion_adjust ( magAlignment , magAlignmentAdjustment ) ;

		}
		else
		{
			dcm_flags._.first_mag_reading = 0 ;
		}

		VectorCopy ( 3 , magFieldEarthNormalizedPrevious , magFieldEarthNormalized ) ;
		VectorCopy ( 9 , rmatPrevious , rmatDelayCompensated ) ;

		dcm_flags._.mag_drift_req = 0 ;
	}
	return ;
}

#endif

#define MAXIMUM_SPIN_DCM_INTEGRAL 20.0 // degrees per second

void PI_feedback(void)
{
	fractional errorRPScaled[3] ;
	int kpyaw ;
	int kprollpitch ;

	// boost the KPs at high spin rate, to compensate for increased error due to calibration error
	// above 50 degrees/second, scale by rotation rate divided by 50

	if ( spin_rate < ( (unsigned int ) ( 50.0 * DEGPERSEC ) ))
	{
		kpyaw = KPYAW ;
		kprollpitch = KPROLLPITCH ;
	}
	else if ( spin_rate < ( (unsigned int ) ( 500.0 * DEGPERSEC ) ))
	{
		kpyaw = ((unsigned int )( KPYAW*8.0 / ( 50.0 * DEGPERSEC )))*(spin_rate>>3) ;
		kprollpitch = ((unsigned int )( KPROLLPITCH*8.0 / ( 50.0 * DEGPERSEC )))*(spin_rate>>3) ;
	}
	else
	{
		kpyaw = ( int ) ( 10.0 * KPYAW ) ;
		kprollpitch = ( int ) ( 10.0 * KPROLLPITCH ) ;
	}
	VectorScale( 3 , omegacorrP , errorYawplane , kpyaw ) ; // Scale gain = 2
	VectorScale( 3 , errorRPScaled , errorRP , kprollpitch ) ; // Scale gain = 2
	VectorAdd( 3 , omegacorrP , omegacorrP , errorRPScaled ) ;

	// turn off the offset integrator while spinning, it doesn't work in that case,
	// and it only causes trouble.

	if ( spin_rate < ( (unsigned int ) ( MAXIMUM_SPIN_DCM_INTEGRAL * DEGPERSEC ) ))
	{	
		gyroCorrectionIntegral[0].WW += ( __builtin_mulss( errorRP[0] , KIROLLPITCH )>>3) ;
		gyroCorrectionIntegral[1].WW += ( __builtin_mulss( errorRP[1] , KIROLLPITCH )>>3) ;
		gyroCorrectionIntegral[2].WW += ( __builtin_mulss( errorRP[2] , KIROLLPITCH )>>3) ;

		gyroCorrectionIntegral[0].WW += ( __builtin_mulss( errorYawplane[0] , KIYAW )>>3) ;
		gyroCorrectionIntegral[1].WW += ( __builtin_mulss( errorYawplane[1] , KIYAW )>>3) ;
		gyroCorrectionIntegral[2].WW += ( __builtin_mulss( errorYawplane[2] , KIYAW )>>3) ;
	}

	omegacorrI[0] = gyroCorrectionIntegral[0]._.W1>>3 ;
	omegacorrI[1] = gyroCorrectionIntegral[1]._.W1>>3 ;
	omegacorrI[2] = gyroCorrectionIntegral[2]._.W1>>3 ;

	return ;
}

unsigned int adjust_gyro_gain ( unsigned int old_gain , int gain_change )
{
	unsigned int gain ;
	gain = old_gain + gain_change ;
	if ( gain > (unsigned int) ( 1.1 * GGAIN ))
	{
		gain = (unsigned int) ( 1.1 * GGAIN ) ;
	}
	if ( gain < (unsigned int) ( 0.9 * GGAIN ))
	{
		gain = (unsigned int) ( 0.9 * GGAIN ) ;
	}
	return gain ;
}

#define GYRO_CALIB_TAU 10.0
#define MINIMUM_SPIN_RATE_GYRO_CALIB 50.0 // degrees/second

void calibrate_gyros(void)
{
	fractional omegacorrPweighted[3] ;
	long calib_accum ;
	int gain_change ;
	unsigned int spin_rate_over2 ;
	if ( spin_rate > ( unsigned int ) ( MINIMUM_SPIN_RATE_GYRO_CALIB * DEGPERSEC ) )
	{
		spin_rate_over2 = spin_rate>>1 ;
		VectorMultiply( 3 , omegacorrPweighted , spin_axis , omegacorrP ) ; // includes 1/2

		calib_accum = __builtin_mulsu( omegacorrPweighted[0] , (unsigned int )( 0.025*GGAIN/GYRO_CALIB_TAU ) ) ;
		gain_change = __builtin_divsd( calib_accum , spin_rate_over2 ) ;
		ggain[0] = adjust_gyro_gain( ggain[0] , gain_change ) ;

		calib_accum = __builtin_mulsu( omegacorrPweighted[1] , (unsigned int )( 0.025*GGAIN/GYRO_CALIB_TAU ) ) ;
		gain_change = __builtin_divsd( calib_accum , spin_rate_over2 ) ;
		ggain[1] = adjust_gyro_gain( ggain[1] , gain_change ) ;

		calib_accum = __builtin_mulsu( omegacorrPweighted[2] , (unsigned int )( 0.025*GGAIN/GYRO_CALIB_TAU ) ) ;
		gain_change = __builtin_divsd( calib_accum , spin_rate_over2 ) ;
		ggain[2] = adjust_gyro_gain( ggain[2] , gain_change ) ;
	}
	return ;
}

/*
void output_matrix(void)
//	This routine makes the direction cosine matrix evident
//	by setting the three servos to the three values in the
//	matrix.
{
	union longww accum ;
	accum.WW = __builtin_mulss( rmat[6] , 4000 ) ;
//	PDC1 = 3000 + accum._.W1 ;
//	accum.WW = __builtin_mulss( rmat[7] , 4000 ) ;
	accum.WW = __builtin_mulss( rmat[3] , 4000 ) ;
	PDC2 = 3000 + accum._.W1 ;
	accum.WW = __builtin_mulss( rmat[4] , 4000 ) ;
	PDC3 = 3000 + accum._.W1 ;
	return ;
}
*/

/*
void output_IMUvelocity(void)
{
	PDC1 = pulsesat( IMUvelocityx._.W1 + 3000 ) ;
	PDC2 = pulsesat( IMUvelocityy._.W1 + 3000 ) ;
	PDC3 = pulsesat( IMUvelocityz._.W1 + 3000 ) ;

//	PDC1 = pulsesat( accelEarth[0] + 3000 ) ;
//	PDC2 = pulsesat( accelEarth[1] + 3000 ) ;
//	PDC3 = pulsesat( accelEarth[2] + 3000 ) ;

	return ;
}
*/

extern void dead_reckon(void) ;

void dcm_run_imu_step(void)
//	update the matrix, renormalize it, 
//	adjust for roll and pitch drift,
//	and send it to the servos.
{
	dead_reckon() ;
//	Lets leave this for a while in case we need to revert roll_pitch_drift

#ifndef NEW_ACCELERATION_COMPENSATION
#if ( HILSIM != 1 )
	adj_accel() ;
#endif
#endif
	rupdate() ;
	normalize() ;
	roll_pitch_drift() ;
#if (MAG_YAW_DRIFT == 1)
	if ( magMessage == 7  )
	{
		mag_drift() ;
	}
	else
	{
		yaw_drift() ;
	}
#else
	yaw_drift() ;
#endif
	PI_feedback() ;
	calibrate_gyros() ;	
	return ;
}

