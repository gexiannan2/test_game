//////////////////////////////////////////////////////////////////////////////////////
// description: represents a 3-dimensional Vector
//
// author: runtimeTerror (runtimeterror@kaiundina.de - http://www.kaiundina.de)
// last modified: 2003-07-30 20-30
//


#include	<math.h>
#include	<iostream>
#ifndef WIN32
#include <cmath>
#endif

#ifndef __VECTOR3D_INCLUDED__
#define __VECTOR3D_INCLUDED__

// Type to use for vector-components (should be double or float)
#define TYPEF float

// often used parameters
#define PARAM_T TYPEF x, TYPEF y, TYPEF z  // 3 Coordinates of TYPEF
#define PARAM_O const CVector3D& v             // 1 Vector-object as alias called v
#define PARAM_P CVector3D* v             // 1 Vector-reference as pointer

// often used combinations of those parameters
#define PARAM_TT TYPEF x1, TYPEF y1, TYPEF z1, TYPEF x2, TYPEF y2, TYPEF z2
#define PARAM_TP TYPEF x1, TYPEF y1, TYPEF z1, CVector3D* v2
#define PARAM_TO TYPEF x1, TYPEF y1, TYPEF z1, CVector3D& v2
#define PARAM_PT CVector3D* v1,              TYPEF x2, TYPEF y2, TYPEF z2
#define PARAM_PP CVector3D* v1,              CVector3D* v2
#define PARAM_PO CVector3D* v1,              CVector3D& v2
#define PARAM_OT CVector3D& v1,              TYPEF x2, TYPEF y2, TYPEF z2
#define PARAM_OP CVector3D& v1,              CVector3D* v2
#define PARAM_OO CVector3D& v1,              CVector3D& v2

const TYPEF    dDegToRad = 3.1415926f / 180.f;
const TYPEF    dRadToDeg = 180.f / 3.1415926f;

const float g_Dir2Vector[8][2] = {
	{0,1},
	{-0.7071f,0.7071f},
	{-1,0},
	{-0.7071f,-0.7071f},
	{0,-1.f},
	{0.7071f,-0.7071f},
	{1,0},
	{0.7071f,0.7071f}
};



// 优化平方根函数
typedef union
{
    int     i;          // as integer
    float   f;          // as float
    struct              // as bit fields
    {
        unsigned int    sign:1;
        unsigned int    biasedexponent:8;
        unsigned int    significand;
    }bits;
}INTORFLOAT;

/* Bias constant used for fast conversions between int and float. First element
   in INTORFLOAT union is integer -- we are storing biased exponent 23, mantissa .1, which
   is equivalent to 1.5 x 2^23. */
const INTORFLOAT  bias = {((23 + 127) << 23) + (1 << 22)};

#define SQRTTABLESIZE       256             /* Note: code below assumes this is 256. */
const unsigned int sqrttable[SQRTTABLESIZE] = 
{
		531980127, 532026288, 532072271, 532118079, 532163712, 532209174, 532254465, 532299589,
		532344546, 532389339, 532433970, 532478440, 532522750, 532566903, 532610900, 532654744,
		532698434, 532741974, 532785365, 532828607, 532871704, 532914655, 532957463, 533000129,
		533042654, 533085041, 533127289, 533169401, 533211378, 533253220, 533294931, 533336509,
		533377958, 533419278, 533460470, 533501535, 533542475, 533583291, 533623984, 533664554,
		533705004, 533745334, 533785545, 533825638, 533865615, 533905476, 533945222, 533984855,
		534024374, 534063782, 534103079, 534142267, 534181345, 534220315, 534259178, 534297934,
		534336585, 534375132, 534413574, 534451914, 534490152, 534528288, 534566324, 534604260,
		534642098, 534679837, 534717478, 534755023, 534792473, 534829827, 534867086, 534904252,
		534941325, 534978305, 535015194, 535051992, 535088699, 535125317, 535161846, 535198287,
		535234640, 535270905, 535307085, 535343178, 535379187, 535415110, 535450950, 535486706,
		535522379, 535557970, 535593480, 535628908, 535664255, 535699523, 535734711, 535769820,
		535804850, 535839803, 535874678, 535909476, 535944198, 535978844, 536013414, 536047910,
		536082331, 536116678, 536150952, 536185153, 536219281, 536253337, 536287322, 536321235,
		536355078, 536388850, 536422553, 536456186, 536489750, 536523246, 536556673, 536590033,
		536623325, 536656551, 536689709, 536722802, 536755829, 536788791, 536821688, 536854520,
		536887280, 536919921, 536952436, 536984827, 537017094, 537049241, 537081267, 537113174,
		537144963, 537176637, 537208195, 537239640, 537270972, 537302193, 537333304, 537364306,
		537395200, 537425987, 537456669, 537487246, 537517720, 537548091, 537578361, 537608530,
		537638600, 537668572, 537698446, 537728224, 537757906, 537787493, 537816986, 537846387,
		537875696, 537904913, 537934040, 537963078, 537992027, 538020888, 538049662, 538078350,
		538106952, 538135470, 538163903, 538192254, 538220521, 538248707, 538276812, 538304837,
		538332781, 538360647, 538388434, 538416144, 538443776, 538471332, 538498812, 538526217,
		538553548, 538580804, 538607987, 538635097, 538662136, 538689102, 538715997, 538742822,
		538769577, 538796263, 538822880, 538849428, 538875909, 538902322, 538928668, 538954949,
		538981163, 539007312, 539033396, 539059416, 539085373, 539111265, 539137095, 539162863,
		539188568, 539214212, 539239794, 539265316, 539290778, 539316180, 539341522, 539366806,
		539392031, 539417197, 539442306, 539467358, 539492352, 539517290, 539542171, 539566997,
		539591768, 539616483, 539641143, 539665749, 539690301, 539714800, 539739245, 539763637,
		539787976, 539812264, 539836499, 539860682, 539884815, 539908896, 539932927, 539956907,
		539980838, 540004718, 540028549, 540052332, 540076065, 540099750, 540123387, 540146976,
		540170517, 540194011, 540217458, 540240858, 540264211, 540287519, 540310780, 540333996,
};

inline float qsqrt( float f )
{
    INTORFLOAT      fi;
    unsigned int    e, n;

    /* Get raw bits of floating point f. */
    fi.f = f;
    n = fi.i;

    /* Divide exponent by 2 -- this is done in-place, no need to shift all
       the way down to 0 the least significant bits and then back up again.
       Note that we are also dividing the exponent bias (127) by 2, this
       gets corrected when we add in the sqrttable entry. */
    e = (n >> 1) & 0x3f800000;

    /* n is the table index -- we're using 1 bit from the original exponent
       (e0) plus 7 bits from the mantissa. */
    n = (n >> 16) & 0xff;

    /* Add calculated exponent to mantissa + re-biasing constant from table. */
    fi.i = e + sqrttable[n];

    /* Return floating point result. */
    return fi.f;
}

// 正弦表 (将浮点数 *1024 整型化)
const int g_nSinBuffer[64] = 
{
	    1024,	1019,	1004,	979,	946,	903,	851,	791,	
        724,	649,	568,	482,	391,	297,	199,	100,	
        0,	   -100,	-199,	-297,	-391,	-482,	-568,	-649,	
        -724,	-791,	-851,	-903,	-946,	-979,	-1004,	-1019,	
        -1024,	-1019,	-1004,	-979,	-946,	-903,	-851,	-791,	
        -724,	-649,	-568,	-482,	-391,	-297,	-199,	-100,	
        0,	     100,	199,	297,	391,	482,	568,	649,	
        724,	791,	851,	903,	946,	979,	1004,	1019
};

inline int	g_GetDistance(int nX1, int nY1, int nX2, int nY2)
{
	INTORFLOAT tmp;
	tmp.f = qsqrt((float)((nX1 - nX2) * (nX1 - nX2) + (nY1 - nY2) * (nY1 - nY2)));
	tmp.f += bias.f;
	tmp.i -= bias.i;
	return tmp.i;
}

const int mindisterr = 30;
inline bool	g_isNearEnough(int nX1, int nY1, int nX2, int nY2)
{
	if (g_GetDistance(nX1, nY1, nX2, nY2) < mindisterr)
		return true;
	return false;
}

inline bool	g_isInRadius(int nX1, int nY1, int nX2, int nY2, int radius)
{
	if (g_GetDistance(nX1, nY1, nX2, nY2) <= radius)
		return true;
	return false;
}

// 8方向(顺时针)转模向量
inline void g_GetVectorByDir(int nDir, float& fX, float& fY)
{
	nDir *= 8;

	if (nDir >= 0 && nDir < 64)
	{
		float fSin = (float)g_nSinBuffer[nDir];
		fY =  fSin / 1024.0f;

		fX = qsqrt((float)(1.0f - fY * fY));

		if (nDir < 32)
			fX = -fX;
	}
}

inline int	g_GetDirIndex(int nX1, int nY1, int nX2, int nY2)
{
	int		nRet = -1;

	if (nX1 == nX2 && nY1 == nY2)
		return -1;

	int		nDistance = g_GetDistance(nX1, nY1, nX2, nY2);
	
	if (nDistance == 0 ) 
		return -1;
	
	int		nYLength = nY2 - nY1;
	int		nSin = (nYLength << 10) / nDistance;	// 放大1024倍
	
	nRet = 0;
	for (int i = 0; i < 32; i++)		// 顺时针方向 从270度到90度，sin值递减
	{
		if (nSin > g_nSinBuffer[i])
			break;
		nRet = i;
	}

	if ((nX2 - nX1) > 0)
	{
		nRet = 63 - nRet;
	}

	return nRet;
}

inline int	g_Dir64To8(int nDir)
{
	return ((nDir + 4) >> 3) & 0x07;
} 

inline int g_InverseDir(int nDir)
{
	nDir += 4; 
	if ( nDir > 7 ) 
		nDir = (nDir - 1) % 7;
	return nDir;
}

inline int g_GetNextDir(bool bClockWise, INT32 nDir)
{
	if (bClockWise)
	{
		nDir++; 
		if ( nDir > 7 )
			nDir = 0;
	}
	else 
	{
		nDir--; 
		if ( nDir < 0 )
			nDir = 7;
	}

	return nDir;
}

class CVector3D 
{

public:
	TYPEF x, y, z; // the 3 vector-components

	//////////////////////////////////////////////////////////////////////////////////////



	// Constructors
	inline CVector3D(){this->x = this->y = this->z = 0;}
	inline CVector3D(TYPEF xyz){this->x = this->y = this->z = xyz;}
	inline CVector3D(PARAM_T){this->x = x;this->y = y;this->z = z;}
	inline CVector3D(PARAM_P){this->x = v->x;this->y = v->y;this->z = v->z;}
	inline CVector3D(PARAM_O){this->x = v.x;this->y = v.y;this->z = v.z;}
	inline TYPEF	GetDirRad( ){return acos( x );}
	inline TYPEF GetDistanceFrom(const CVector3D* v) const { return qsqrt((x-v->x)*(x-v->x)+(y-v->y)*(y-v->y)+(z-v->z)*(z-v->z)); }
	inline TYPEF GetDistanceFrom2(const CVector3D* v) const { return (x-v->x)*(x-v->x)+(y-v->y)*(y-v->y)+(z-v->z)*(z-v->z); }
	inline void MakeByDir(char nDir)
  {
		y = 0;
		x = g_Dir2Vector[(int)nDir][0]; z = g_Dir2Vector[(int)nDir][1];
	}
	
	inline bool CanWatch(TYPEF fSight, const CVector3D* pcTarget)const
    {
	     CVector3D cDistance;
	     cDistance.x = pcTarget->x - this->x;
	     cDistance.y = pcTarget->y - this->y;
	     cDistance.z = pcTarget->z - this->z;
	     TYPEF tDistance = cDistance.sqr();
	     if (tDistance < fSight * fSight)
	     {
	         return true;
         }
        return false;
    }


			//! Rotates the vector by a specified number of degrees around the Y axis and the specified center.
		/** \param degrees Number of degrees to rotate around the Y axis.
		\param center The center of the rotation. */
	//void RotateXZBy(DOUBLE degrees, const CVector3D<T>& center=CVector3D<T>())
	void RotateXZBy(TYPEF degrees, const CVector3D& center = CVector3D())
	{
//	    degrees += degrees;
		degrees *= dDegToRad;
		TYPEF cs = cosf(degrees);
		TYPEF sn = sinf(degrees);
		x -= center.x;
		z -= center.z;
		TYPEF tTempX = x;
		TYPEF tTempZ = z;
		x = (tTempX * cs - tTempZ * sn);
		z = (tTempX * sn + tTempZ * cs);
		x += center.x;
		z += center.z;
	}

//	FLOAT DistanceToLine(const CVector3D *pcLine)
//	{
//	    FLOAT fDis = abs(x * pcLine->x + z * pcLine->z) / (sqrt(pcLine->x * pcLine->x + pcLine->z * pcLine->z));
//	}

	// returns result in a new instance
	CVector3D operator + (PARAM_P){return CVector3D(this->x + v->x, this->y + v->y, this->z + v->z);}
	CVector3D operator + (PARAM_O){return  CVector3D(this->x + v.x, this->y + v.y, this->z + v.z);}
	CVector3D* operator += (PARAM_P) { this->x += v->x; this->y += v->y; this->z += v->z; return this; }
	CVector3D* operator += (PARAM_O) { this->x += v.x;  this->y += v.y;  this->z += v.z;  return this; }
	CVector3D operator - (PARAM_P)  {
		TYPEF tx = this->x - v->x;
		TYPEF ty = this->y - v->y;
		TYPEF tz = this->z - v->z;
#ifndef WIN32
		if (isnan(tx)) tx = 0;
		if (isnan(ty)) ty = 0;
		if (isnan(tz)) tz = 0;
#else
		if (_isnan(tx)) tx = 0;
		if (_isnan(ty)) ty = 0;
		if (_isnan(tz)) tz = 0;
#endif
		return CVector3D(tx,  ty,  tz); 
	}
	CVector3D operator - (PARAM_O)  { 
		TYPEF tx = this->x - v.x;
		TYPEF ty = this->y - v.y;
		TYPEF tz = this->z - v.z;
#ifndef WIN32
		if (isnan(tx)) tx = 0;
		if (isnan(ty)) ty = 0;
		if (isnan(tz)) tz = 0;
#else
		if (_isnan(tx)) tx = 0;
		if (_isnan(ty)) ty = 0;
		if (_isnan(tz)) tz = 0;
#endif
		return CVector3D(tx,  ty,  tz); 
	}
	CVector3D* operator -= (PARAM_P) { 
		TYPEF tx = this->x - v->x;
		TYPEF ty = this->y - v->y;
		TYPEF tz = this->z - v->z;
#ifndef WIN32
		if (isnan(tx)) tx = 0;
		if (isnan(ty)) ty = 0;
		if (isnan(tz)) tz = 0;
#else
		if (_isnan(tx)) tx = 0;
		if (_isnan(ty)) ty = 0;
		if (_isnan(tz)) tz = 0;
#endif
		x = tx; y =ty; z = tz;
		return this;
	}
	CVector3D* operator -= (PARAM_O) { 
		TYPEF tx = this->x - v.x;
		TYPEF ty = this->y - v.y;
		TYPEF tz = this->z - v.z;
#ifndef WIN32
		if (isnan(tx)) tx = 0;
		if (isnan(ty)) ty = 0;
		if (isnan(tz)) tz = 0;
#else
		if (_isnan(tx)) tx = 0;
		if (_isnan(ty)) ty = 0;
		if (_isnan(tz)) tz = 0;
#endif
		x = tx; y =ty; z = tz;
		return this;
	}
	TYPEF operator * (PARAM_P)  { return this->x * v->x  + this->y * v->y  + this->z * v->z;  }
	TYPEF operator * (PARAM_O)  { return this->x * v.x   + this->y * v.y   + this->z * v.z;   }
	CVector3D operator *  (TYPEF s)          { return CVector3D(this->x * s, this->y * s, this->z * s); }
	CVector3D* operator *= (TYPEF s)         { this->x *= s; this->y *= s; this->z *= s; return this; }
	CVector3D operator / (TYPEF s)          { return CVector3D(this->x / s, this->y / s, this->z / s); }
	CVector3D* operator /= (TYPEF s)         { this->x /= s; this->y /= s; this->z /= s; return this; }
	bool operator == (PARAM_P)  { if (this->x != v->x ) return false; if (this->y != v->y ) return false; if (this->z != v->z ) return false; return true;  }
	bool operator == (PARAM_O)  { if (this->x != v.x  ) return false; if (this->y != v.y  ) return false; if (this->z != v.z  ) return false; return true;  }
	bool operator != (PARAM_P)  { if (this->x != v->x ) return true;  if (this->y != v->y ) return true;  if (this->z != v->z ) return true;  return false; }
	bool operator != (PARAM_O)  { if (this->x != v.x  ) return true;  if (this->y != v.y  ) return true;  if (this->z != v.z  ) return true;  return false; }
	CVector3D* operator = (PARAM_P) { this->x = v->x; this->y = v->y; this->z = v->z; return this; }
	CVector3D* operator = (PARAM_O) { 
		this->x = v.x;  this->y = v.y;  this->z = v.z;  return this; 
	}


	TYPEF sqr ()        { return this->x * this->x + this->y * this->y + this->z * this->z; }


	///////////////////////////////////////////////////
	// scales a vector to ensure, it has a length of 1.0
	// vector mustn't be (0.0, 0.0, 0.0)
	static CVector3D unit (PARAM_T){ return antiscale(x, y, z, length(x, y, z)); }
	static CVector3D unit (PARAM_P){ return antiscale(v, length(v)); }
	static CVector3D unit (PARAM_O){ return antiscale(v, length(v)); }
	CVector3D* unit (){ return this->antiscale(this->length()); }

	///////////////////////////////////////////////////
	// multiplies (scales) a vector with the reziprocal value of a number (1/s)
	// s mustn't be 0
	static CVector3D antiscale (PARAM_T, TYPEF s){ return CVector3D(x       / s, y       / s, z       / s); }
	static CVector3D antiscale (PARAM_P, TYPEF s){ return CVector3D(v->x    / s, v->y    / s, v->z    / s); }
	static CVector3D antiscale (PARAM_O, TYPEF s){ return CVector3D(v.x / s, v.y / s, v.z / s); }
	CVector3D* antiscale (TYPEF s){ this->x /= s; this->y /= s; this->z /= s; return this; }

	///////////////////////////////////////////////////
	// computes the length of a vector
	static TYPEF length (PARAM_T){ return (TYPEF)sqrt((double)(x * x) + (double)(y * y) + (double)(z * z)); }
	static TYPEF length (PARAM_P){ return (TYPEF)sqrt((double)(v->x * v->x) + (double)(v->y * v->y) + (double)(v->z * v->z)); }
	static TYPEF length (PARAM_O){ return (TYPEF)sqrt((double)(v.x * v.x) + (double)(v.y * v.y) + (double)(v.z * v.z)); }
	TYPEF length ()const{ return (TYPEF)sqrt((double)(this->x * this->x) + (double)(this->y * this->y) + (double)(this->z * this->z)); }
	inline void normalise()
	{
		float fLength = length();
		if (!(fLength < 0.00000001f && fLength > -0.00000001f))
		{
			float fInvLength = 1.f / fLength;
			x *= fInvLength;
			y *= fInvLength;
			z *= fInvLength;
		}
	}


	///////////////////////////////////////////////////
	// determines the smallest angle between two vectors
	// none of the vectors must be (0.0, 0.0, 0.0)

	// returns result using two external vectors
	TYPEF angle (PARAM_TT) { return (TYPEF)acos((double)(x1    * x2    + y1    * y2    + z1    * z2   ) / sqrt((double)(CVector3D(x1, y1, z1).sqr() * CVector3D(x2, y2, z2).sqr()))); }
	TYPEF angle (PARAM_TP) { return (TYPEF)acos((double)(x1    * v2->x + y1    * v2->y + z1    * v2->z) / sqrt((double)(CVector3D(x1, y1, z1).sqr() * v2->sqr()                 ))); }
	TYPEF angle (PARAM_TO) { return (TYPEF)acos((double)(x1    * v2.x  + y1    * v2.y  + z1    * v2.z ) / sqrt((double)(CVector3D(x1, y1, z1).sqr() * v2.sqr()                  ))); }
	TYPEF angle (PARAM_PT) { return (TYPEF)acos((double)(v1->x * x2    + v1->y * y2    + v1->z * z2   ) / sqrt((double)(v1->sqr() * CVector3D(x2, y2, z2).sqr()))); }
	TYPEF angle (PARAM_PP) { return (TYPEF)acos((double)(v1->x * v2->x + v1->y * v2->y + v1->z * v2->z) / sqrt((double)(v1->sqr() * v2->sqr()	))); }
	TYPEF angle (PARAM_PO) { return (TYPEF)acos((double)(v1->x * v2.x  + v1->y * v2.y  + v1->z * v2.z ) / sqrt((double)(v1->sqr()	* v2.sqr()	))); }
	TYPEF angle (PARAM_OT) { return (TYPEF)acos((double)(v1.x  * x2    + v1.y  * y2    + v1.z  * z2   ) / sqrt((double)(v1.sqr()* CVector3D(x2, y2, z2).sqr()))); }
	TYPEF angle (PARAM_OP) { return (TYPEF)acos((double)(v1.x  * v2->x + v1.y  * v2->y + v1.z  * v2->z) / sqrt((double)(v1.sqr()	* v2->sqr()	))); }
	TYPEF angle (PARAM_OO) { return (TYPEF)acos((double)(v1.x  * v2.x  + v1.y  * v2.y  + v1.z  * v2.z ) / sqrt((double)(v1.sqr()	* v2.sqr()	))); }

	// applied with the current instance (no changes will be made)
	TYPEF angle (PARAM_T) { return (TYPEF)acos((double)(this->x * x    + this->y * y    + this->z * z   ) / sqrt((double)(this->sqr() * CVector3D(x, y, z).sqr()))); }
	TYPEF angle (PARAM_P) { return (TYPEF)acos((double)(this->x * v->x + this->y * v->y + this->z * v->z) / sqrt((double)(this->sqr() * v->sqr()))); }
//	TYPEF angle (PARAM_O) { return (TYPEF)acos((double)(this->x * v.x  + this->y * v.y  + this->z * v.z ) / sqrt((double)(this->sqr() * v.sqr()))); }


	/////////////////////////////////////////////////////
	//// determines the cosine of the smallest angle between two vectors
	//// none of the vectors must be (0.0, 0.0, 0.0)

	//// returns result using two external vectors
	//TYPEF cos (PARAM_TT) { return (x1    * x2    + y1    * y2    + z1    * z2   ) / (TYPEF)sqrt((double)(CVector3D(x1, y1, z1).sqr() * CVector3D(x2, y2, z2).sqr())); }
	//TYPEF cos (PARAM_TP) { return (x1    * v2->x + y1    * v2->y + z1    * v2->z) / (TYPEF)sqrt((double)(CVector3D(x1, y1, z1).sqr() * v2->sqr()	)); }
	//TYPEF cos (PARAM_TO) { return (x1    * v2.x  + y1    * v2.y  + z1    * v2.z ) / (TYPEF)sqrt((double)(CVector3D(x1, y1, z1).sqr() * v2.sqr()	)); }
	//TYPEF cos (PARAM_PT) { return (v1->x * x2    + v1->y * y2    + v1->z * z2   ) / (TYPEF)sqrt((double)(v1->sqr()	* CVector3D(x2, y2, z2).sqr())); }
	//TYPEF cos (PARAM_PP) { return (v1->x * v2->x + v1->y * v2->y + v1->z * v2->z) / (TYPEF)sqrt((double)(v1->sqr()	* v2->sqr()	)); }
	//TYPEF cos (PARAM_PO) { return (v1->x * v2.x  + v1->y * v2.y  + v1->z * v2.z ) / (TYPEF)sqrt((double)(v1->sqr()	* v2.sqr()	)); }
	//TYPEF cos (PARAM_OT) { return (v1.x  * x2    + v1.y  * y2    + v1.z  * z2   ) / (TYPEF)sqrt((double)(v1.sqr()	* CVector3D(x2, y2, z2).sqr())); }
	//TYPEF cos (PARAM_OP) { return (v1.x  * v2->x + v1.y  * v2->y + v1.z  * v2->z) / (TYPEF)sqrt((double)(v1.sqr()	* v2->sqr()	)); }
	//TYPEF cos (PARAM_OO) { return (v1.x  * v2.x  + v1.y  * v2.y  + v1.z  * v2.z ) / (TYPEF)sqrt((double)(v1.sqr()	* v2.sqr()	)); }

	//// applied with the current instance (no changes will be made)
	//TYPEF cos (PARAM_T) { return (this->x * x    + this->y * y    + this->z * z   ) / (TYPEF)sqrt((double)(this->sqr() * CVector3D(x, y, z).sqr())); }
	//TYPEF cos (PARAM_P) { return (this->x * v->x + this->y * v->y + this->z * v->z) / (TYPEF)sqrt((double)(this->sqr() * v->sqr())); }
	////TYPEF cos (PARAM_O) { return (this->x * v.x  + this->y * v.y  + this->z * v.z ) / (TYPEF)sqrt((double)(this->sqr() * v.sqr())); }

	/////////////////////////////////////////////////////
	//// determines the sine of the smallest angle between two vectors
	//// none of the vectors must be (0.0, 0.0, 0.0)

	//// returns result using two external vectors
	//TYPEF sin (PARAM_TT) { return (TYPEF)sqrt((double)(CVector3D(y1    * z2    - z1    * y2,    z1    * x2    - x1    * z2,    x1    * y2    - y1    * x2   ).sqr() / (CVector3D(x1, y1, z1).sqr() * CVector3D(x2, y2, z2).sqr()))); }
	//TYPEF sin (PARAM_TP) { return (TYPEF)sqrt((double)(CVector3D(y1    * v2->z - z1    * v2->y, z1    * v2->x - x1    * v2->z, x1    * v2->y - y1    * v2->x).sqr() / (CVector3D(x1, y1, z1).sqr() * v2->sqr()                 ))); }
	//TYPEF sin (PARAM_TO) { return (TYPEF)sqrt((double)(CVector3D(y1    * v2.z  - z1    * v2.y,  z1    * v2.x  - x1    * v2.z,  x1    * v2.y  - y1    * v2.x ).sqr() / (CVector3D(x1, y1, z1).sqr() * v2.sqr()                  ))); }
	//TYPEF sin (PARAM_PT) { return (TYPEF)sqrt((double)(CVector3D(v1->y * z2    - v1->z * y2,    v1->z * x2    - v1->x * z2,    v1->x * y2    - v1->y * x2   ).sqr() / (v1->sqr()	* CVector3D(x2, y2, z2).sqr()))); }
	//TYPEF sin (PARAM_PP) { return (TYPEF)sqrt((double)(CVector3D(v1->y * v2->z - v1->z * v2->y, v1->z * v2->x - v1->x * v2->z, v1->x * v2->y - v1->y * v2->x).sqr() / (v1->sqr()	* v2->sqr()	))); }
	//TYPEF sin (PARAM_PO) { return (TYPEF)sqrt((double)(CVector3D(v1->y * v2.z  - v1->z * v2.y,  v1->z * v2.x  - v1->x * v2.z,  v1->x * v2.y  - v1->y * v2.x ).sqr() / (v1->sqr()	* v2.sqr()	))); }
	//TYPEF sin (PARAM_OT) { return (TYPEF)sqrt((double)(CVector3D(v1.y  * z2    - v1.z  * y2,    v1.z  * x2    - v1.x  * z2,    v1.x  * y2    - v1.y  * x2   ).sqr() / (v1.sqr()	* CVector3D(x2, y2, z2).sqr()))); }
	//TYPEF sin (PARAM_OP) { return (TYPEF)sqrt((double)(CVector3D(v1.y  * v2->z - v1.z  * v2->y, v1.z  * v2->x - v1.x  * v2->z, v1.x  * v2->y - v1.y  * v2->x).sqr() / (v1.sqr()	* v2->sqr()	))); }
	//TYPEF sin (PARAM_OO) { return (TYPEF)sqrt((double)(CVector3D(v1.y  * v2.z  - v1.z  * v2.y,  v1.z  * v2.x  - v1.x  * v2.z,  v1.x  * v2.y  - v1.y  * v2.x ).sqr() / (v1.sqr()	* v2.sqr()))); }

	//// applied with the current instance (no changes will be made)
	//TYPEF sin (PARAM_T) { return (TYPEF)sqrt((double)(CVector3D(this->y * z    - this->z * y,    this->z * x    - this->x * z,    this->x * y    - this->y * x   ).sqr() / (this->sqr() * CVector3D(x, y, z).sqr()))); }
	//TYPEF sin (PARAM_P) { return (TYPEF)sqrt((double)(CVector3D(this->y * v->z - this->z * v->y, this->z * v->x - this->x * v->z, this->x * v->y - this->y * v->x).sqr() / (this->sqr() * v->sqr()	))); }
	////TYPEF sin (PARAM_O) { return (TYPEF)sqrt((double)(CVector3D(this->y * v.z  - this->z * v.y,  this->z * v.x  - this->x * v.z,  this->x * v.y  - this->y * v.x ).sqr() / (this->sqr() * v.sqr()	))); }


	inline const float& operator[] (int i) const {
		return ((float*)this)[i];
	}

	inline float& operator[] (int i) {
		return ((float*)this)[i];
	}

};

#endif
