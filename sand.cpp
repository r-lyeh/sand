/* Sand is a lightweight and simple time framework written in C++11.
 * Sand supports Unix stamps, hires timers, calendars, locales and tweening.
 * Copyright (c) 2010-2013 Mario 'rlyeh' Rodriguez

 * Based on code by Robert Penner, GapJumper, Terry Schubring, Jesus Gollonet,
 * Tomas Cepeda, John Resig. Thanks guys! :-)

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.

 * issues:
 * - if year < 1900, sand::rtc() behavior is undefined

 * to do:
 * - grain -> struct grain { std::uint64_t seconds, fract; };
 * - looper/rtc -> serialize factor as well (and held?)
 * - move factor/shift and pause/resume to dt
 * - kiloseconds, ks | ref: http://bavardage.github.com/Kiloseconds/
 * - something like http://momentjs.com/ for pretty printing
 * - also, https://code.google.com/p/datejs/

 * - rlyeh ~~ listening to The Mission / Butterfly on a wheel
 */

#include <cassert>
#include <cmath>
#include <ctime>

#include <deque>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sand.hpp"

#if defined(_WIN32) || defined(_WIN64)
#   include <Windows.h>
#   define $windows $yes
#   define kSandTimerHandle                      LARGE_INTEGER
#   define kSandTimerFreq( handle )              { kSandTimerHandle fhandle; DWORD_PTR oldmask = ::SetThreadAffinityMask(::GetCurrentThread(), 0); ::QueryPerformanceFrequency( &fhandle ); ::SetThreadAffinityMask(::GetCurrentThread(), oldmask); frequency = 1000000.0 / double(fhandle.QuadPart); }
#   define kSandTimerUpdate( handle )            {                          DWORD_PTR oldmask = ::SetThreadAffinityMask(::GetCurrentThread(), 0); ::QueryPerformanceCounter  ( &handle ); ::SetThreadAffinityMask(::GetCurrentThread(), oldmask); }
#   define kSandTimerSetCounter( handle, value ) handle.QuadPart = value
#   define kSandTimerDiffCounter( handle1, handle2 ) ( ( handle2.QuadPart - handle1.QuadPart ) * frequency )
#   define kSandTimerSleep( seconds_f )          Sleep( (int)(seconds_f * 1000) )
#   define kSandTimerWink( units_t )             Sleep( units_t )
#else
#   include <sys/time.h>
#   define $unix    $yes
//  hmmm... check clock_getres() as seen in http://tdistler.com/2010/06/27/high-performance-timing-on-linux-windows#more-350
//  frequency int clock_getres(clockid_t clock_id, struct timespec *res);
//  clock     int clock_gettime(clockid_t clock_id, struct timespec *tp);
//  nanosleep() instead?
#   define kSandTimerHandle                      timeval
#   define kSandTimerFreq( handle )
#   define kSandTimerUpdate( handle )            gettimeofday( &handle, NULL )
#   define kSandTimerSetCounter( handle, value ) do { handle.tv_sec = 0; handle.tv_usec = value; } while (0)
#   define kSandTimerDiffCounter( handle1, handle2 ) ( (handle2.tv_sec * 1000000.0) + handle2.tv_usec ) - ( (handle1.tv_sec * 1000000.0) + handle1.tv_usec )
#   define kSandTimerSleep( seconds_f )          usleep( seconds_f * 1000000.f )
#   define kSandTimerWink( units_t )             usleep( units_t )
//  do { float fractpart, intpart; fractpart = std::modf( seconds_f, &intpart); \
//    ::sleep( int(intpart) ); usleep( int(fractpart * 1000000) ); } while( 0 )
#endif

#ifdef  $windows
#define $welse $no
#else
#define $welse $yes
#endif

#ifdef  $unix
#define $uelse $no
#else
#define $uelse $yes
#endif

#define $yes(...) __VA_ARGS__
#define $no(...)

namespace sand
{
namespace
{
    int floor( double f ) {
        return int( ::floor(f) );
    }
    int ceil( double f ) {
        return int( ::ceil(f) );
    }
    class custom : public std::string
    {
        public:

        custom() : std::string()
        {}

        template<typename T>
        custom( const T &t ) : std::string()
        {
            std::stringstream ss;
            if( ss << t )
                this->assign( ss.str() );
        }

        std::deque<custom> tokenize( const std::string &chars ) const
        {
            std::string map( 256, '\0' );

            for( auto it = chars.begin(), end = chars.end(); it != end; ++it )
                map[ *it ] = '\1';

            std::deque<custom> tokens;

            tokens.push_back( custom() );

            for( int i = 0, end = this->size(); i < end; ++i )
            {
                unsigned char c = at(i);

                std::string &str = tokens.back();

                if( !map.at(c) )
                    str.push_back( c );
                else
                if( str.size() )
                    tokens.push_back( custom() );
            }

            while( tokens.size() && !tokens.back().size() )
                tokens.pop_back();

            return tokens;
        }

        template <typename T>
        T as() const
        {
            T t;
            std::stringstream ss;
            ss << *this;
            return ss >> t ? t : T();
        }
    };

    namespace legacy
    {
        auto epoch = [](){
            return std::chrono::system_clock::to_time_t( std::chrono::system_clock::now() );
        };

        class dt
        {
            public:

            dt()
            {
                reset();
            }

            double s() //const
            {
                return us() / 1000000.0;
            }

            double ms() //const
            {
                return us() / 1000.0;
            }

            double us() //const
            {
                kSandTimerUpdate( endCount );

                return kSandTimerDiffCounter( startCount, endCount );
            }

            double ns() //const
            {
                return us() * 1000.0;
            }

            void reset()
            {
                kSandTimerFreq( frequency ); //to dt2() ctor ?

                kSandTimerSetCounter( startCount, 0 );
                kSandTimerSetCounter( endCount, 0 );

                kSandTimerUpdate( startCount );
            }

            protected:

            kSandTimerHandle startCount, endCount;
            double frequency;
        };
    }
}
}

namespace sand
{
    namespace
    {
        double offset = 0;
        const double app_epoch = double( std::time(NULL) );
        legacy::dt local;
    }

    double now() {
        return offset + local.s() + app_epoch;
    }

    double runtime() {
        return offset + local.s();
    }

    void lapse( double t ) {
        offset += t;
    }

    std::string format( uint64_t timestamp_secs, const std::string &locale )
    {
        try {
            std::string locale; // = "es-ES", "Chinese_China.936", "en_US.UTF8", etc...
            std::time_t t = timestamp_secs;
                std::tm tm = *std::localtime(&t);
            std::stringstream ss;
#if 1
                ss.imbue( std::locale( locale.empty() ? std::locale("").name() : locale ) );
                ss << std::put_time( &tm, "%c" );
#else
#   ifdef _MSC_VER
                // msvc crashes on %z and %Z
                ss << std::put_time( &tm, "%Y-%m-%d %H:%M:%S" );
#   else
                ss << std::put_time( &tm, "%Y-%m-%d %H:%M:%S %z" );
#   endif
#endif
            return ss.str();
        }
        catch(...) {
        }
        return std::string();
    }

    void wink() {
        kSandTimerWink( 1 );
    }

    void sleep( double seconds ) {
        kSandTimerSleep( seconds );
        return;

        std::chrono::microseconds duration( (int)(seconds * 1000000) );
        std::this_thread::sleep_for( duration );
    }

    double nanoseconds( double t ) {
        return t / 1000000000.0;
    }
    double microseconds( double t ) {
        return t / 1000000.0;
    }
    double milliseconds( double t ) {
        return t / 1000.0;
    }
    double seconds( double t ) {
        return t;
    }
    double minutes( double t ) {
        return t * seconds(60);
    }
    double hours( double t ) {
        return t * minutes(60);
    }
    double days( double t ) {
        return t * hours(24);
    }
    double weeks( double t ) {
        return t * days(7);
    }
    double years( double t ) {
        return t * days(365.242190402); // + days(n/4);
    }

    double to_nanoseconds( double t ) {
        return t * 1000000000.0;
    }
    double to_microseconds( double t ) {
        return t * 1000000.0;
    }
    double to_milliseconds( double t ) {
        return t * 1000.0;
    }
    double to_seconds( double t ) {
        return t;
    }
    double to_minutes( double t ) {
        return t / seconds(60);
    }
    double to_hours( double t ) {
        return t / minutes(60);
    }
    double to_days( double t ) {
        return t / hours(24);
    }
    double to_weeks( double t ) {
        return t / days(7);
    }
    double to_years( double t ) {
        return t / days(365.242190402); // + days(n/4);
    }


    float ping( float dt01 ) {
        return dt01;
    }
    float pong( float dt01 ) {
        return 1 - dt01;
    }
    float pingpong( float dt01 ) {
        return dt01 < 0.5f ? dt01 + dt01 : 2 - dt01 - dt01;
    }
    // triangle sinus... useful? {
    float sinus( float dt01 ) {
        float x4 = dt01 + dt01 + dt01 + dt01;

        if( x4 >= 3 ) return x4 - 4;
        if( x4  < 1 ) return x4;
        return 2 - x4;
    }
    // }
    float linear( float dt01 ) {
        return dt01;
    }

    float tween( int tweener_type, float current )
    {
        const bool looped = false;
        const float pi = 3.1415926535897932384626433832795f;
        const float d = /*total=*/ 1.f;

        float t = current;

        /* small optimization { */

        if( d <= 0.f || t <= 0.f )
        {
            return 0.f;
        }

        if( t >= d )
        {
            if( looped )
            {
                // todo: optimize me?
                while( t >= d )
                    t -= d;
            }
            else
            {
                return 1.f;
            }
        }

        /* } */

        switch( tweener_type )
        {
        case TYPE::LINEAR_01:
            {
                return t/d;
            }

        case TYPE::SINPI2_01:
            {
                float fDelta = t/d;
                return std::sin(fDelta * 0.5f * pi);
            }

        case TYPE::ACELBREAK_01:
            {
                float fDelta = t/d;
                return (std::sin((fDelta * pi) - (pi * 0.5f)) + 1.0f) * 0.5f;
            }

        case TYPE::BACKIN_01:
            {
                float s = 1.70158f;
                float postFix = t/=d;
                return postFix * t * ((s + 1) * t - s);
            }

        case TYPE::BACKOUT_01:
            {
                float s = 1.70158f;
                return 1.f * ((t = t/d-1)*t*((s+1)*t + s) + 1);
            }

        case TYPE::BACKINOUT_01:
            {
                float s = 1.70158f;
                if ((t/=d/2) < 1)
                    return 1.f/2*(t*t*(((s*=(1.525f))+1)*t - s));

                float postFix = t-=2;
                return 1.f/2*((postFix)*t*(((s*=(1.525f))+1)*t + s) + 2);
            }

#       define $BOUNCE(v) \
        if ((t/=d) < (1/2.75f)) \
            { \
            v = 1.f*(7.5625f*t*t); \
            } \
        else if (t < (2/2.75f)) \
            { \
            float postFix = t-=(1.5f/2.75f); \
            \
            v = 1.f*(7.5625f*(postFix)*t + .75f); \
            } \
        else if (t < (2.5/2.75)) \
            { \
            float postFix = t-=(2.25f/2.75f); \
            \
            v = 1.f*(7.5625f*(postFix)*t + .9375f); \
            } \
        else \
            { \
            float postFix = t-=(2.625f/2.75f); \
            \
            v = 1.f*(7.5625f*(postFix)*t + .984375f); \
            }

        case TYPE::BOUNCEIN_01:
            {
                float v;
                t = d-t;
                $BOUNCE(v);
                return 1.f - v;
            }

        case TYPE::BOUNCEOUT_01:
            {
                float v;
                $BOUNCE(v);
                return v;
            }

        case TYPE::BOUNCEINOUT_01:
            {
                float v;
                if (t < d/2) {
                    t = t*2;
                    t = d-t;
                    $BOUNCE(v);
                    return (1.f - v) * .5f;
                } else {
                    t = t*2 -d;
                    $BOUNCE(v);
                    return v * .5f + 1.f*.5f;
                }
            }

#       undef $BOUNCE

        case TYPE::CIRCIN_01:
            t /= d;
            return 1.f - std::sqrt(1 - t*t);

        case TYPE::CIRCOUT_01:
            t /= d;
            t--;
            return std::sqrt(1 - t*t);

        case TYPE::CIRCINOUT_01:
            t /= d/2;
            if(t < 1)
                return -1.f/2 * (std::sqrt(1 - t*t) - 1);

            t-=2;
            return 1.f/2 * (std::sqrt(1 - t*t) + 1);


        case TYPE::ELASTICIN_01:
            {
                t/=d;

                float p=d*.3f;
                float a=1.f;
                float s=p/4;
                float postFix =a*std::pow(2,10*(t-=1)); // this is a fix, again, with post-increment operators

                return -(postFix * std::sin((t*d-s)*(2*pi)/p ));
            }

        case TYPE::ELASTICOUT_01:
            {
                float p=d*.3f;
                float a=1.f;
                float s=p/4;

                return (a*std::pow(2,-10*t) * std::sin( (t*d-s)*(2*pi)/p ) + 1.f);
            }

        case TYPE::ELASTICINOUT_01:
            {
                t/=d/2;

                float p=d*(.3f*1.5f);
                float a=1.f;
                float s=p/4;

                if (t < 1) {
                    float postFix =a*std::pow(2,10*(t-=1)); // postIncrement is evil
                    return -.5f*(postFix* std::sin( (t*d-s)*(2*pi)/p ));
                }

                float postFix =  a*std::pow(2,-10*(t-=1)); // postIncrement is evil
                return postFix * std::sin( (t*d-s)*(2*pi)/p )*.5f + 1.f;
            }

        case TYPE::EXPOIN_01:
            return std::pow(2, 10 * (t/d - 1));

        case TYPE::EXPOOUT_01:
            return 1.f - ( t == d ? 0 : std::pow(2, -10.f * (t/d)));

        case TYPE::EXPOINOUT_01:
            t /= d/2;
            if (t < 1)
                return 1.f/2 * std::pow(2, 10 * (t - 1));

            t--;
            return 1.f/2 * (-std::pow(2, -10 * t) + 2);

        case TYPE::QUADIN_01:
            t /= d;
            return t*t;

        case TYPE::QUADOUT_01:
            t /= d;
            return (2.f - t) * t;

        case TYPE::QUADINOUT_01:
            t /= d/2;
            if(t < 1)
                return (1.f/2*t*t);

            t--;
            return -1.f/2 * (t*(t-2) - 1);

        case TYPE::CUBICIN_01:
            t /= d;
            return t*t*t;

        case TYPE::CUBICOUT_01:
            t /= d;
            t--;
            return (1.f + t*t*t);

        case TYPE::CUBICINOUT_01:
            t /= d/2;
            if (t < 1)
                return 1.f/2*t*t*t;

            t -= 2;
            return 1.f/2*(t*t*t + 2);

        case TYPE::QUARTIN_01:
            t /= d;
            return t*t*t*t;

        case TYPE::QUARTOUT_01:
            t /= d;
            t--;
            return (1.f - t*t*t*t);

        case TYPE::QUARTINOUT_01:
            t /= d/2;
            if(t < 1)
                return 1.f/2*t*t*t*t;

            t -= 2;
            return -1.f/2 * (t*t*t*t - 2);

        case TYPE::QUINTIN_01:
            t /= d;
            return t*t*t*t*t;

        case TYPE::QUINTOUT_01:
            t /= d;
            t--;
            return (1.f + t*t*t*t*t);

        case TYPE::QUINTINOUT_01:
            t /= d/2;
            if(t < 1)
                return 1.f/2*t*t*t*t*t;

            t -= 2;
            return 1.f/2*(t*t*t*t*t + 2);

        case TYPE::SINEIN_01:
            return 1.f - std::cos(t/d * (pi/2));

        case TYPE::SINEOUT_01:
            return std::sin(t/d * (pi/2));

        case TYPE::SINEINOUT_01:
            return -1.f/2 * (std::cos(pi*t/d) - 1);


        case TYPE::SINESQUARE:
            {
                float A = std::sin(0.5f*(t/d)*pi);
                return A*A;
            }

        case TYPE::EXPONENTIAL:
            {
                return 1/(1+std::exp(6-12*(t/d)));
            }

        case TYPE::SCHUBRING1:
            {
                t /= d;
                return 2*(t+(0.5f-t)*std::abs(0.5f-t))-0.5f;
            }

        case TYPE::SCHUBRING2:
            {
                t /= d;
                float p1pass= 2*(t+(0.5f-t)*std::abs(0.5f-t))-0.5f;
                float p2pass= 2*(p1pass+(0.5f-p1pass)*std::abs(0.5f-p1pass))-0.5f;
                return p2pass;
            }

        case TYPE::SCHUBRING3:
            {
                t /= d;
                float p1pass= 2*(t+(0.5f-t)*std::abs(0.5f-t))-0.5f;
                float p2pass= 2*(p1pass+(0.5f-p1pass)*std::abs(0.5f-p1pass))-0.5f;
                float pAvg=(p1pass+p2pass)/2;
                return pAvg;
            }

        default:
            return 0;
        }
    }

    // tweeners memoization
#   define $with(fn,type) float fn( float dt01 ) { \
        enum { SLOTS = 256 }; \
        static std::vector<float> lut; \
        if( lut.empty() ) \
            for( int i = 0; i < SLOTS; ++i ) \
                lut.push_back( tween( TYPE::type, float(i) / (SLOTS-1) ) ); \
        return dt01 < 0 ? 0 : dt01 >= 1 ? 1 : lut[ int(dt01*(SLOTS-1)) ]; \
    }

    $with( quadin, QUADIN_01 )
    $with( quadout, QUADOUT_01 )
    $with( quadinout, QUADINOUT_01 )
    $with( cubicin, CUBICIN_01 )
    $with( cubicout, CUBICOUT_01 )
    $with( cubicinout, CUBICINOUT_01 )
    $with( quartin, QUARTIN_01 )
    $with( quartout, QUARTOUT_01 )
    $with( quartinout, QUARTINOUT_01 )
    $with( quintin, QUINTIN_01 )
    $with( quintout, QUINTOUT_01 )
    $with( quintinout, QUINTINOUT_01 )
    $with( sinein, SINEIN_01 )
    $with( sineout, SINEOUT_01 )
    $with( sineinout, SINEINOUT_01 )
    $with( expoin, EXPOIN_01 )
    $with( expoout, EXPOOUT_01 )
    $with( expoinout, EXPOINOUT_01 )
    $with( circin, CIRCIN_01 )
    $with( circout, CIRCOUT_01 )
    $with( circinout, CIRCINOUT_01 )
    $with( elasticin, ELASTICIN_01 )
    $with( elasticout, ELASTICOUT_01 )
    $with( elasticinout, ELASTICINOUT_01 )
    $with( backin, BACKIN_01 )
    $with( backout, BACKOUT_01 )
    $with( backinout, BACKINOUT_01 )
    $with( bouncein, BOUNCEIN_01 )
    $with( bounceout, BOUNCEOUT_01 )
    $with( bounceinout, BOUNCEINOUT_01 )

    $with( sinesquare, SINESQUARE )
    $with( exponential, EXPONENTIAL )

    $with( terrys1, SCHUBRING1 )
    $with( terrys2, SCHUBRING2 )
    $with( terrys3, SCHUBRING3 )

    $with( acelbreak, ACELBREAK_01 )
    $with( sinpi2, SINPI2_01 )

#   undef $with

    const char *str( int type )
    {
        switch(type)
        {
        default:
#       define $with( unused, type ) case TYPE::type: return #type;

        $with( quadin, UNDEFINED )

        $with( quadin, LINEAR_01 )

        $with( quadin, QUADIN_01 )
        $with( quadout, QUADOUT_01 )
        $with( quadinout, QUADINOUT_01 )
        $with( cubicin, CUBICIN_01 )
        $with( cubicout, CUBICOUT_01 )
        $with( cubicinout, CUBICINOUT_01 )
        $with( quartin, QUARTIN_01 )
        $with( quartout, QUARTOUT_01 )
        $with( quartinout, QUARTINOUT_01 )
        $with( quintin, QUINTIN_01 )
        $with( quintout, QUINTOUT_01 )
        $with( quintinout, QUINTINOUT_01 )
        $with( sinein, SINEIN_01 )
        $with( sineout, SINEOUT_01 )
        $with( sineinout, SINEINOUT_01 )
        $with( expoin, EXPOIN_01 )
        $with( expoout, EXPOOUT_01 )
        $with( expoinout, EXPOINOUT_01 )
        $with( circin, CIRCIN_01 )
        $with( circout, CIRCOUT_01 )
        $with( circinout, CIRCINOUT_01 )
        $with( elasticin, ELASTICIN_01 )
        $with( elasticout, ELASTICOUT_01 )
        $with( elasticinout, ELASTICINOUT_01 )
        $with( backin, BACKIN_01 )
        $with( backout, BACKOUT_01 )
        $with( backinout, BACKINOUT_01 )
        $with( bouncein, BOUNCEIN_01 )
        $with( bounceout, BOUNCEOUT_01 )
        $with( bounceinout, BOUNCEINOUT_01 )

        $with( sinesquare, SINESQUARE )
        $with( exponential, EXPONENTIAL )

        $with( terrys1, SCHUBRING1 )
        $with( terrys2, SCHUBRING2 )
        $with( terrys3, SCHUBRING3 )

        $with( acelbreak, ACELBREAK_01 )
        $with( sinpi2, SINPI2_01 )

#       undef $with
        }
    }
}

namespace sand
{
    // object time (in seconds.microseconds)
    time_t rtc::time_obj()
    {
        return creation + time_t( factor * dt.s() );
    }

    time_t rtc::elapsed()
    {
        return time_t( factor * dt.s() );
    }

    rtc::rtc() : factor( 1.0 )
    {
        set( double( std::time( 0 ) ) );
    }

    rtc::rtc( const std::string &import ) : factor( 1.0 )
    {
        this->str( import );
    }

    void rtc::reset() //useful?
    {
        set( 0 );
    }

    void rtc::set( const double &t )
    {
        held = false;
        creation = time_t( t );
        dt.reset();
    }

    void rtc::shift( double f ) // factor(), phase(), speed() instead?
    {
        assert( f > 0.0 );

        factor = f;
    }

    void rtc::pause()
    {
        held = true;
    }

    double rtc::resume()
    {
        held = false;

        return double( time_obj() - creation );
    }

    double rtc::update()
    {
        //return double( creation = ( held ? creation : time_obj() ) );

        if( held )
            return double( creation );

        set( double( creation ) + elapsed() );

        return double( creation );
    }

    double rtc::get() const
    {
        return double( creation );
    }

    rtc::operator double() const
    {
        return get();
    }

    std::string rtc::format( const std::string &fmt ) const
    {
        char pBuffer[80];

        struct tm * timeinfo;
        time_t stored = (time_t)( get() );
        timeinfo = localtime ( &stored );
        strftime(pBuffer, 80, fmt.c_str(), timeinfo);

        return pBuffer;
    }

    int rtc::Y() const
    {
        return custom( format("%Y") ).as<int>();
    }
    int rtc::M() const
    {
        return custom( format("%m") ).as<int>();
    }
    int rtc::D() const
    {
        return custom( format("%d") ).as<int>();
    }

    int rtc::h() const
    {
        return custom( format("%H") ).as<int>();
    }
    int rtc::m() const
    {
        return custom( format("%M") ).as<int>();
    }
    int rtc::s() const
    {
        return custom( format("%S") ).as<int>();
    }

    std::string rtc::str() const
    {
        return format( "%Y-%m-%d %H:%M:%S" );
    }

    void rtc::str( const std::string& import )
    {
        std::deque< custom > token = custom( import ).tokenize(":-/ "); //:)

        //assert( token.size() >= 6 );

        if( token.size() < 6 )
        {
            set( 0 );
            return;
        }

        struct tm timeinfo;

        //months are in [0..] range where days, hours, mins and secs use [1..] (doh!)
        timeinfo.tm_year  = token[0].as<int>() - 1900;
        timeinfo.tm_mon   = token[1].as<int>() - 1;
        timeinfo.tm_mday  = token[2].as<int>();
        timeinfo.tm_hour  = token[3].as<int>();
        timeinfo.tm_min   = token[4].as<int>();
        timeinfo.tm_sec   = token[5].as<int>();
        //-1 = do not adjust daylight savings
        timeinfo.tm_isdst = -1;

        factor = 1.0; //ahem
        set( double( mktime( &timeinfo ) ) );
    }
}

namespace sand
{
    fps::fps() : frames(0), frames_per_second(0), format("0 fps")
    {}

    bool fps::tick()
    {
        frames++;

        history.push_back( frame_timer.s() );
        if( history.size() > 60*2 ) history.pop_front();
        frame_timer.reset();

        double sec = dt.s();
        if( sec >= 0.5 )
        {
            frames_per_second = frames,
            frames = 0;

            format = ( frames_per_second >= 1 || frames_per_second == 0 ?
                std::to_string( int( frames_per_second / sec ) ) + " fps" :
                std::to_string( int( sec / frames_per_second ) ) + " spf" );

            frames_per_second /= sec;

            dt.reset();

            return true;
        }

        return false;
    }

    void fps::wait( double frames_per_second )
    {
        // @todo: [evaluate] http://gafferongames.com/game-physics/fix-your-timestep/

        if( frames_per_second > 0 )
        {
            double seconds = 1.0/frames_per_second;
            if( seconds > 1 ) seconds = 1;
            do sand::wink(); while( frame_limiter.s() < seconds ); //yield()?
            frame_limiter.reset();
        }
    }

    std::deque< float > fps::get_history() const
    {
        return history;
    }

    std::string fps::str() const
    {
        return format;
    }

    size_t fps::get() const
    {
        return frames_per_second;
    }
}

namespace sand
{
    std::string ago( double diff_seconds ) {
        // based on code by John Resig (jquery.com)
        int diff = abs(diff_seconds);
        int day_diff = sand::floor(diff / 86400);

        if( day_diff == 0 ) {
            if( diff_seconds <   60 ) return "just now";
            if( diff_seconds <  120 ) return "a minute ago";
            if( diff_seconds < 3600 ) return std::to_string(sand::floor(diff/60)) + " minutes ago";
            if( diff_seconds < 7200 ) return "an hour ago";
            return std::to_string( sand::floor(diff/3600) ) + " hours ago";
        }
        if( day_diff ==  1 ) return "yesterday";
        if( day_diff <= 13 ) return std::to_string(day_diff) + " days ago";
        if( day_diff  < 31 ) return std::to_string(sand::ceil(day_diff/7)) + " weeks ago";
        if( day_diff  < 62 ) return "a month ago";
        if( day_diff < 365 ) return std::to_string(sand::ceil(day_diff/31)) + " months ago";
        if( day_diff < 730 ) return "a year ago";
        return std::to_string(sand::ceil(day_diff/365)) + " years ago";
    }

    std::string in( double diff_seconds ) {
        // based on code by John Resig (jquery.com)
        int diff = abs(diff_seconds);
        int day_diff = sand::floor(diff / 86400);

        if( day_diff == 0 ) {
            if( diff_seconds <   60 ) return "right now";
            if( diff_seconds <  120 ) return "in a minute";
            if( diff_seconds < 3600 ) return std::string("in ") + std::to_string(sand::floor(diff/60)) + " minutes";
            if( diff_seconds < 7200 ) return "in an hour";
            return std::string("in ") + std::to_string( sand::floor(diff/3600) ) + " hours ago";
        }
        if( day_diff ==  1 ) return "tomorrow";
        if( day_diff <= 13 ) return std::string("in ") + std::to_string(day_diff) + " days";
        if( day_diff  < 31 ) return std::string("in ") + std::to_string(sand::ceil(day_diff/7)) + " weeks";
        if( day_diff  < 62 ) return "in a month";
        if( day_diff < 365 ) return std::string("in ") + std::to_string(sand::ceil(day_diff/31)) + " months";
        if( day_diff < 730 ) return "in a year";
        return std::string("in ") + std::to_string(sand::ceil(day_diff/365)) + " years";
    }
}

#undef kSandTimerWink
#undef kSandTimerHandle
#undef kSandTimerFreq
#undef kSandTimerUpdate
#undef kSandTimerDiffCounter
#undef kSandTimerSetCounter
#undef kSandTimerGetCounter
#undef kSandTimerSleep
#undef kSandTimerWink

#undef $yes
#undef $no
#undef $uelse
#undef $unix
#undef $welse
#undef $windows