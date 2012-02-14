///////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2011, Matthew Pratt, licensed under the GPL3.
//
// Authors: Matthew Pratt
//
// Date:  7 Feb 2011
//
///////////////////////////////////////////////////////////////////////////////

#ifndef DS1820_H
#define DS1820_H

float ds1820_get_temperature();

void DS1820Skip(void);
void DS1820Convert (void);
void DS1820Init (void);
void DS1820ReadTemp(void);

#endif
