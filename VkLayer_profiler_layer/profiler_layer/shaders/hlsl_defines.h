#pragma once
#ifndef INCLUDED_FROM_HLSL

#define REGISTER( ... )
#define REGISTER_SPACE( ... )
#define SEMANTIC( ... )

#else // INCLUDED_FROM_HLSL

#define REGISTER( LOC )                 register( LOC )
#define REGISTER_SPACE( LOC, SPACE )    register( LOC, SPACE )
#define SEMANTIC( NAME )                :NAME

#endif
