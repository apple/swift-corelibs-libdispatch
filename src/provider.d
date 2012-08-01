/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

typedef struct dispatch_object_s *dispatch_object_t;
typedef struct dispatch_queue_s *dispatch_queue_t;
typedef void (*dispatch_function_t)(void *);

provider dispatch {
	probe queue__push(dispatch_queue_t queue, const char *label,
			dispatch_object_t item, const char *kind,
			dispatch_function_t function, void *context);
	probe queue__pop(dispatch_queue_t queue, const char *label,
			dispatch_object_t item, const char *kind,
			dispatch_function_t function, void *context);
	probe callout__entry(dispatch_queue_t queue, const char *label,
			dispatch_function_t function, void *context);
	probe callout__return(dispatch_queue_t queue, const char *label,
			dispatch_function_t function, void *context);
};

#pragma D attributes Evolving/Evolving/Common provider dispatch provider
#pragma D attributes Private/Private/Common provider dispatch module
#pragma D attributes Private/Private/Common provider dispatch function
#pragma D attributes Evolving/Evolving/Common provider dispatch name
#pragma D attributes Evolving/Evolving/Common provider dispatch args
