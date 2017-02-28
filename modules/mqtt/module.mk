#
# module.mk
#
# Copyright (C) 2017 Erdem MEYDANLI
#

MOD		:= mqtt
$(MOD)_SRCS	+= mqtt.c
$(MOD)_LFLAGS	+= -L$(SYSROOT)/usr/local/lib -lpaho-mqtt3c -lcjson

include mk/mod.mk
