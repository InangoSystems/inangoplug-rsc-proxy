################################################################################
#
#  Copyright 2020-2021 Inango Systems Ltd.
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
################################################################################
EXEFILE ?= tcp-proxy
PREFIX ?=.
CROSS ?=
COMPILER ?=gcc
FILES = tcp_proxy.c

PWD=`pwd`
INCLUDES=-I$(PWD)

all: tcp-proxy

tcp-proxy: $(FILES)
	$(CC) $(LDFLAGS) $(CFLAGS) -std=gnu11 $(INCLUDES) $(FILES) -o $(PREFIX)/$(EXEFILE)

clean:
	rm -vf $(PREFIX)/$(EXEFILE)* *.o

%.o:%.c
	$(CROSS)$(COMPILER) $(INCLUDES) -w -fmax-errors=0 $< -o $@

