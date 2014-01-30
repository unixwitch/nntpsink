/* nntpsink: dummy NNTP server */
/* 
 * Copyright (c) 2013-2014 Felicity Tarnell.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#ifndef	NNTPSINK_H_INCLUDED
#define	NNTPSINK_H_INCLUDED

#include	<sys/types.h>

#include	"setup.h"

void	*xcalloc(size_t, size_t);
void	*xmalloc(size_t);

#endif	/* !NNTPSINK_H_INCLUDED */
