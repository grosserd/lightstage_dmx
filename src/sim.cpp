#include "sim.h"

namespace ls
{
static V3 SrgbToLin( const V3& c ) { return V3( powf( c.x, 2.2f ), powf( c.y, 2.2f ), powf( c.z, 2.2f ) ); }
static float Lum( const V3& c ) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
static float HashT( long long n )
{
	uint32_t h = (uint32_t)( n * 2654435761u ) ^ 0x9e3779b9u;
	h          = ( h ^ ( h >> 15 ) ) * 2246822519u;
	h ^= h >> 13;
	return ( h & 0xffff ) / 65535.0f;
}

M4 FixtureBase( const FixtureInst& f ) { return Mul( Translate( f.pos ), RotEuler( f.rot ) ); }

V3 BodyExtents( const FixtureDef& d )
{
	if( d.kind == Kind::MovingHead || d.kind == Kind::Scanner ) return V3( d.dims.x, d.dims.y, d.dims.z );
	return V3( d.dims.x, d.dims.z, d.dims.y );
}

void BeamRay( const FixtureInst& f, int head, V3& origin, V3& dir, V3& up )
{
	M4 b = FixtureBase( f );
	if( !f.def )
	{
		origin = XformP( b, V3( 0, 0.2f, 0 ) );
		dir    = Norm( XformD( b, V3( 0, 1, 0 ) ) );
		up     = Norm( XformD( b, V3( 0, 0, 1 ) ) );
		return;
	}
	const FixtureDef& d = *f.def;
	V3 e                = BodyExtents( d );
	if( d.kind == Kind::MovingHead || d.kind == Kind::Scanner )
	{
		float pan  = ( f.invertPan ? -1.0f : 1.0f ) * ( f.st.pan + f.st.panSpin );
		float tilt = ( f.invertTilt ? -1.0f : 1.0f ) * ( f.st.tilt + f.st.tiltSpin );
		M4 h       = Mul( b, Mul( RotY( pan * kDeg ), Mul( Translate( V3( 0, e.y * 0.62f, 0 ) ), RotX( tilt * kDeg ) ) ) );
		origin     = XformP( h, V3( 0, e.y * 0.3f, 0 ) );
		dir        = Norm( XformD( h, V3( 0, 1, 0 ) ) );
		up         = Norm( XformD( h, V3( 0, 0, 1 ) ) );
		return;
	}
	V3 lp( 0, e.y, 0 );
	if( head >= 0 && head < (int)d.pixels.size() )
	{
		const V3& p = d.pixels[ head ].pos;
		lp          = V3( p.x * e.x * 0.92f, e.y, p.z * e.z * 0.92f );
	}
	origin = XformP( b, lp );
	dir    = Norm( XformD( b, V3( 0, 1, 0 ) ) );
	up     = Norm( XformD( b, V3( 0, 0, 1 ) ) );
}

void SimulateFixture( const Show& show, FixtureInst& f, float dt, double t )
{
	FixtureState& s = f.st;
	const Mode* m   = f.M();
	if( !m || !f.def )
	{
		s.intensity = 0;
		s.heads.assign( 1, V3( 0, 0, 0 ) );
		return;
	}
	const FixtureDef& d = *f.def;
	int nh              = d.Heads();
	int n               = (int)m->ch.size();
	std::vector< uint16_t > v( n );
	for( int i = 0; i < n; ++i )
		v[ i ] = m->ch[ i ].isFine ? 0 : show.Get16( f, i );

	bool hasDimmer = false, hasEmit = false, hasShutter = false;
	float dim  = 1.0f;
	std::vector< float > hDim( nh, -1.0f );
	std::vector< V3 > hEmit( nh, V3( 0, 0, 0 ) );
	std::vector< bool > hEmitSet( nh, false );
	V3 gEmit( 0, 0, 0 );
	V3 maxEmit( 0, 0, 0 );//sum of all emitters at full (for normalization)
	V3 cmy( 0, 0, 0 );
	V3 wheelFilter( 1, 1, 1 );
	V3 macro( -1, -1, -1 );
	float kelvinTint = 0;
	float shutter    = 1.0f;
	int strobeKind   = 0;//0 none, 1 strobe, 2 pulse, 3 ramp up, 4 ramp down, 5 random
	float strobeHz   = 0;
	float ptSpeed    = 1.0f;
	float panT = s.panTarget, tiltT = s.tiltTarget;
	float panSpinSpeed = 0, tiltSpinSpeed = 0;
	bool hasZoom = false;
	float zoom   = d.beamMin;
	float focus = 0.5f, frost = 0, iris = 1, fog = 0;
	int gobo = 0, prism = 0;
	float goboShake = 0;
	bool goboAngleMode = false, prismAngleMode = false;
	float goboAngle = 0, goboSpeed = 0, prismAngle = 0, prismSpeed = 0;
	int effectKind = 0;
	float effectSpeed = 0;
	int wheelCounter  = 0;

	auto addEmit = [ & ]( const Channel& c, V3 col ) {
		if( c.pixels.empty() )
			gEmit += col;
		else
			for( int p : c.pixels )
				if( p < nh ) { hEmit[ p ] += col; hEmitSet[ p ] = true; }
	};

	for( int i = 0; i < n; ++i )
	{
		const Channel* c = &m->ch[ i ];
		if( c->isFine || c->attr == Attr::None ) continue;
		//switching channel: pick the alternative selected by the controlling channel
		if( c->swCtrl >= 0 && !c->swAlts.empty() )
		{
			const Cap* cc = m->ch[ c->swCtrl ].Find( v[ c->swCtrl ] );
			if( cc )
				for( auto& sw : cc->sw )
					if( sw.first == c->name || sw.first == c->key )
						for( auto& alt : c->swAlts )
							if( alt.first == sw.second ) c = alt.second.get();
		}
		uint16_t val = v[ i ];
		if( IsEmitter( c->attr ) )
		{
			V3 ec = EmitterColor( c->attr );
			if( d.subtractive && ( c->attr == Attr::Cyan || c->attr == Attr::Magenta || c->attr == Attr::Yellow ) )
			{
				const Cap* cp = c->Find( val );
				float lv      = cp && cp->k == CapK::ColorIntensity ? Saturate( cp->Val( val ) ) : 0.0f;
				if( c->attr == Attr::Cyan ) cmy.x = std::max( cmy.x, lv );
				if( c->attr == Attr::Magenta ) cmy.y = std::max( cmy.y, lv );
				if( c->attr == Attr::Yellow ) cmy.z = std::max( cmy.z, lv );
				continue;
			}
			hasEmit = true;
			const Cap* cp = c->Find( val );
			float lv      = cp && ( cp->k == CapK::ColorIntensity || cp->k == CapK::Intensity ) ? Saturate( cp->Val( val ) ) : 0.0f;
			addEmit( *c, ec * lv );
			continue;
		}
		const Cap* cp = c->Find( val );
		if( !cp ) continue;
		float x = cp->Val( val );
		switch( cp->k )
		{
		case CapK::Intensity:
			if( c->attr == Attr::Dimmer || c->attr == Attr::Generic || c->attr == Attr::Shutter )
			{
				if( c->pixels.empty() ) { dim *= Saturate( x ); hasDimmer = true; }
				else
					for( int p : c->pixels )
						if( p < nh ) hDim[ p ] = ( hDim[ p ] < 0 ? 1.0f : hDim[ p ] ) * Saturate( x );
			}
			break;
		case CapK::ShutterOpen: hasShutter = true; break;
		case CapK::ShutterClosed: hasShutter = true; shutter = 0; break;
		case CapK::Strobe: hasShutter = true; strobeKind = 1; strobeHz = x; break;
		case CapK::StrobePulse: hasShutter = true; strobeKind = 2; strobeHz = x; break;
		case CapK::StrobeRampUp: hasShutter = true; strobeKind = 3; strobeHz = x; break;
		case CapK::StrobeRampDown: hasShutter = true; strobeKind = 4; strobeHz = x; break;
		case CapK::StrobeRandom: hasShutter = true; strobeKind = 5; strobeHz = x; break;
		case CapK::Pan:
		case CapK::Tilt:
		{
			float mn = 1e9f, mx = -1e9f;
			for( auto& o : c->caps )
				if( o.k == cp->k ) { mn = std::min( { mn, o.a, o.b } ); mx = std::max( { mx, o.a, o.b } ); }
			float ang = x - ( mn + mx ) * 0.5f;
			if( cp->k == CapK::Pan ) panT = ang;
			else tiltT = ang;
			break;
		}
		case CapK::PanCont: panSpinSpeed = x * 360.0f; break;
		case CapK::TiltCont: tiltSpinSpeed = x * 360.0f; break;
		case CapK::PanTiltSpeed: ptSpeed = Clamp( x, 0.03f, 1.0f ); break;
		case CapK::WheelSlot:
		case CapK::WheelShake:
		case CapK::WheelRot:
		{
			if( cp->wheel < 0 || cp->wheel >= (int)d.wheels.size() ) break;
			const Wheel& w = d.wheels[ cp->wheel ];
			int ns         = (int)w.slots.size();
			if( ns == 0 ) break;
			float slot;//0-based fractional
			int wi = std::min( wheelCounter++, 3 );
			if( cp->k == CapK::WheelRot )
			{
				s.wheelPos[ wi ] += x * dt * ns;
				s.wheelPos[ wi ] = fmodf( s.wheelPos[ wi ], (float)ns );
				if( s.wheelPos[ wi ] < 0 ) s.wheelPos[ wi ] += ns;
				slot = s.wheelPos[ wi ];
			}
			else
			{
				slot = Lerp( cp->slotA, cp->slotB, cp->T( val ) ) - 1.0f;
				s.wheelPos[ wi ] = slot;
			}
			if( cp->k == CapK::WheelShake ) goboShake = x;
			int s0     = ( (int)floorf( slot ) % ns + ns ) % ns, s1 = ( s0 + 1 ) % ns;
			float fr   = slot - floorf( slot );
			auto& A    = w.slots[ s0 ];
			auto& B    = w.slots[ s1 ];
			if( w.IsColor() )
			{
				V3 ca = A.type == SlotT::Color && A.hasColor ? SrgbToLin( A.color ) : V3( 1, 1, 1 );
				V3 cb = B.type == SlotT::Color && B.hasColor ? SrgbToLin( B.color ) : V3( 1, 1, 1 );
				if( A.type == SlotT::Closed ) ca = V3( 0, 0, 0 );
				if( B.type == SlotT::Closed ) cb = V3( 0, 0, 0 );
				//split colours: hard edge in the middle of the transition (approximation: blend)
				V3 cw = fr < 0.02f ? ca : ( fr > 0.98f ? cb : ca * ( 1 - fr ) + cb * fr );
				wheelFilter = wheelFilter * cw;
			}
			else if( w.IsPrism() )
			{
				const WheelSlot& S = fr < 0.5f ? A : B;
				if( S.type == SlotT::Prism ) prism = std::max( 3, S.facets );
			}
			else
			{
				const WheelSlot& S = fr < 0.5f ? A : B;
				if( S.type == SlotT::Gobo || S.type == SlotT::Anim ) gobo = S.gobo;
				else if( S.type == SlotT::Closed ) shutter = 0;
				else if( S.type == SlotT::Color && S.hasColor ) wheelFilter = wheelFilter * SrgbToLin( S.color );
				else if( S.type == SlotT::Prism ) prism = std::max( 3, S.facets );
				else if( S.type == SlotT::Frost ) frost = std::max( frost, 0.8f );
			}
			break;
		}
		case CapK::WheelSlotRot:
			if( cp->angleMode ) { goboAngleMode = true; goboAngle = x; }
			else goboSpeed = x;
			break;
		case CapK::ColorPreset:
			if( !cp->colors.empty() )
			{
				V3 acc( 0, 0, 0 );
				for( auto& col : cp->colors ) acc += SrgbToLin( col );
				acc = acc / (float)cp->colors.size();
				if( !cp->colorsEnd.empty() )
				{
					V3 acc2( 0, 0, 0 );
					for( auto& col : cp->colorsEnd ) acc2 += SrgbToLin( col );
					acc2 = acc2 / (float)cp->colorsEnd.size();
					float tt = cp->T( val );
					acc      = acc * ( 1 - tt ) + acc2 * tt;
				}
				macro = acc;
			}
			break;
		case CapK::ColorTemp:
			if( c->attr == Attr::CTO || c->attr == Attr::ColorMacro ) kelvinTint = x;
			break;
		case CapK::Zoom: hasZoom = true; zoom = x; break;
		case CapK::Focus: focus = Saturate( x ); break;
		case CapK::Iris: iris = Saturate( x ); break;
		case CapK::Frost: frost = std::max( frost, Saturate( x ) ); break;
		case CapK::Prism:
			prism = std::max( 3, cp->facets );
			if( cp->angleMode ) { prismAngleMode = true; prismAngle = x; }
			else if( cp->a != 0 || cp->b != 0 ) prismSpeed = x;
			break;
		case CapK::PrismRot:
			if( cp->angleMode ) { prismAngleMode = true; prismAngle = x; }
			else prismSpeed = x;
			break;
		case CapK::Fog: fog = std::max( fog, Saturate( x ) ); break;
		case CapK::ColorJump: effectKind = 1; effectSpeed = x; break;
		case CapK::ColorFade: effectKind = 2; effectSpeed = x; break;
		default: break;
		}
	}
	(void)hasShutter;

	//normalization: full mix of all emitters = 1 on the brightest channel
	{
		//distinct emitter types present
		bool seen[ (int)Attr::Count ] = {};
		for( auto& c : m->ch )
			if( !c.isFine && IsEmitter( c.attr ) && !seen[ (int)c.attr ] && !( d.subtractive && ( c.attr == Attr::Cyan || c.attr == Attr::Magenta || c.attr == Attr::Yellow ) ) )
			{
				seen[ (int)c.attr ] = true;
				maxEmit += EmitterColor( c.attr );
			}
	}
	float norm = 1.0f / std::max( 1.0f, std::max( { maxEmit.x, maxEmit.y, maxEmit.z } ) );

	//base colour
	V3 white = Kelvin( d.kelvin );
	V3 base;
	if( hasEmit )
		base = gEmit * norm;
	else
		base = white;
	if( d.subtractive )
		base = base * V3( 1 - 0.97f * cmy.x, 1 - 0.97f * cmy.y, 1 - 0.97f * cmy.z );
	if( macro.x >= 0 )
		base = hasEmit ? macro : base * macro;
	if( effectKind && effectSpeed > 0 )
	{
		s.effectPhase += effectSpeed * dt;
		float h = effectKind == 1 ? floorf( s.effectPhase * 7.0f ) / 7.0f : s.effectPhase;
		base    = Hsv( h, 1, 1 );
		base    = SrgbToLin( base );
	}
	base = base * wheelFilter;
	if( kelvinTint > 0 )
	{
		V3 kt = Kelvin( kelvinTint );
		V3 k0 = Kelvin( d.kelvin );
		base  = base * V3( kt.x / std::max( 0.05f, k0.x ), kt.y / std::max( 0.05f, k0.y ), kt.z / std::max( 0.05f, k0.z ) );
		float mx = std::max( { base.x, base.y, base.z, 1.0f } );
		base = base / mx;
	}

	//shutter / strobe
	float strobe = 1.0f;
	if( strobeKind && strobeHz > 0.01f )
	{
		s.strobePhase += strobeHz * dt;
		long long period = (long long)floor( s.strobePhase );
		float ph         = (float)( s.strobePhase - (double)period );
		switch( strobeKind )
		{
		case 1:
		{
			float duty = std::min( 0.5f, 0.03f * strobeHz + 0.02f );
			strobe     = ( ph < duty || period != s.strobeLast ) ? 1.0f : 0.0f;
			break;
		}
		case 2: strobe = 0.5f - 0.5f * cosf( ph * 2 * kPi ); break;
		case 3: strobe = ph; break;
		case 4: strobe = 1 - ph; break;
		case 5:
		{
			float duty = std::min( 0.5f, 0.03f * strobeHz + 0.02f );
			strobe     = ( HashT( period ) > 0.45f && ( ph < duty || period != s.strobeLast ) ) ? 1.0f : 0.0f;
			break;
		}
		}
		s.strobeLast = period;
	}
	bool halogen = ( d.kind == Kind::Dimmer || d.kind == Kind::Blinder ) && d.kelvin < 3600;
	float gdim   = hasDimmer ? dim : 1.0f;
	float target = gdim * shutter * strobe * std::min( show.master, 1.0f );//above 100 % the renderer boosts the light output
	if( halogen )
	{
		//thermal lag of an incandescent filament
		float k     = target > s.smoothDim ? 14.0f : 7.0f;
		s.smoothDim = Lerp( s.smoothDim, target, 1.0f - expf( -k * dt ) );
	}
	else
		s.smoothDim = target;
	float curve = [ & ]( float x ) { return powf( Saturate( x ), 1.8f ); }( s.smoothDim );

	//heads
	s.heads.resize( nh );
	V3 sum( 0, 0, 0 );
	for( int h = 0; h < nh; ++h )
	{
		V3 c = hEmitSet[ h ] ? ( hEmit[ h ] + gEmit ) * norm : base;
		if( hEmitSet[ h ] )
		{
			if( d.subtractive ) c = c * V3( 1 - 0.97f * cmy.x, 1 - 0.97f * cmy.y, 1 - 0.97f * cmy.z );
			c = c * wheelFilter;
		}
		float hd = hDim[ h ] >= 0 ? powf( hDim[ h ], 1.8f ) : 1.0f;
		//halogen cells: per-cell lag folded into the global lag (cells usually share it)
		s.heads[ h ] = c * ( hd * curve );
		sum += s.heads[ h ];
	}
	V3 avg     = sum / (float)nh;
	float mx   = std::max( { avg.x, avg.y, avg.z } );
	s.color    = mx > 1e-5f ? avg / mx : base;
	s.intensity = mx;
	if( !hasEmit && !hasDimmer && d.kind != Kind::Hazer )
		s.intensity = curve;

	//motion
	float maxV = 330.0f * ptSpeed;//deg/s
	if( !s.init )
	{
		s.pan = panT;
		s.tilt = tiltT;
		s.zoom = hasZoom ? zoom : ( f.beam > 0.5f ? f.beam : d.beamMin );
		s.init = true;
	}
	auto approach = [ & ]( float& cur, float tgt, float k ) {
		float vel = ( tgt - cur ) * k;
		vel       = Clamp( vel, -maxV, maxV );
		float step = vel * dt;
		if( fabsf( step ) > fabsf( tgt - cur ) ) cur = tgt;
		else cur += step;
	};
	s.panTarget  = panT;
	s.tiltTarget = tiltT;
	approach( s.pan, panT, 7.0f );
	approach( s.tilt, tiltT, 7.0f );
	s.panSpin  = fmodf( s.panSpin + panSpinSpeed * dt, 360.0f );
	s.tiltSpin = fmodf( s.tiltSpin + tiltSpinSpeed * dt, 360.0f );
	s.zoomTarget = hasZoom ? zoom : ( f.beam > 0.5f ? f.beam : d.beamMin );
	s.zoom       = Lerp( s.zoom, s.zoomTarget, 1.0f - expf( -9.0f * dt ) );
	s.focus      = focus;
	s.frost      = frost;
	s.iris       = iris;
	s.gobo       = gobo;
	s.goboShake  = goboShake;
	if( goboAngleMode ) s.goboRot = goboAngle * kDeg;
	else
	{
		s.goboSpinAcc += goboSpeed * 2 * kPi * dt;
		s.goboRot = s.goboSpinAcc;
	}
	if( goboShake > 0 ) s.goboRot += 0.25f * sinf( (float)t * goboShake * 2 * kPi );
	s.prism = prism;
	if( prismAngleMode ) s.prismRot = prismAngle * kDeg;
	else
	{
		s.prismSpinAcc += prismSpeed * 2 * kPi * dt;
		s.prismRot = s.prismSpinAcc;
	}
	s.fog = d.kind == Kind::Hazer || fog > 0 ? fog * ( hasDimmer ? dim : 1.0f ) : 0.0f;

}

void Simulate( Show& show, SimGlobals& g, float dt )
{
	g.time += dt;
	show.UpdateMounts();
	float fog = 0;
	for( auto& f : show.fixtures )
	{
		SimulateFixture( show, f, dt, g.time );
		fog += f.st.fog;
	}
	g.hazeTarget = Saturate( show.stage.haze + fog * 0.6f );
	//haze builds up slowly and dissipates slower
	float k = g.hazeTarget > g.haze ? 0.35f : 0.12f;
	g.haze  = Lerp( g.haze, g.hazeTarget, 1.0f - expf( -k * dt ) );
}
}//namespace ls
