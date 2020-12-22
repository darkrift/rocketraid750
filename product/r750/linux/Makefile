# $Id: Makefile,v 1.6 2009/12/11 00:47:48 wsw Exp $
#
# Copyright (C) 2012 HighPoint Technologies, Inc.
# All Rights Reserved.
#

HPT_ROOT := ../../..

C_DEFINES := -DSUPPORT_ARRAY 

TARGETNAME := r750
TARGETTYPE := KMOD
TARGETMODS := him_r750.o ldm_la.o
TARGETOBJS := os_linux.o osm_linux.o div64.o hptinfo.o config.o

include $(HPT_ROOT)/inc/linux_64mpa/Makefile.def

$(TARGETOBJS): osm_linux.h
