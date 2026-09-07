// Copyright 2026 Kakehashi Project
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define KH_EXPORT __attribute__((visibility("default")))
#define KH_DISPATCH_TIME_FOREVER UINT64_MAX
#define KH_QOS_CLASS_DEFAULT 0x15u

typedef void *dispatch_queue_t;
typedef void *dispatch_semaphore_t;
typedef unsigned int qos_class_t;

extern char _dispatch_main_q;
extern intptr_t dispatch_semaphore_signal(dispatch_semaphore_t dsema);
extern intptr_t dispatch_semaphore_wait(dispatch_semaphore_t dsema,
		uint64_t timeout);
extern void dispatch_assert_queue(dispatch_queue_t queue);

struct kh_objc_class {
	uint64_t isa;
	uint64_t superclass;
	uint64_t name;
	uint64_t instance_size;
	uint32_t flags;
	uint32_t reserved;
	uint64_t methods;
	uint64_t ivars;
	uint64_t cache;
};

extern void *objc_getClass(const char *name);
extern void *kh_objc_class_register(const uint8_t *name, void *superclass,
		void *class_storage, void *metaclass_storage, size_t instance_size,
		uint32_t kind);

KH_EXPORT struct kh_objc_class kh_dispatch_queue_class
		__asm("_OBJC_CLASS_$_OS_dispatch_queue") = { 0 };
KH_EXPORT struct kh_objc_class kh_dispatch_queue_metaclass
		__asm("_OBJC_METACLASS_$_OS_dispatch_queue") = { 0 };

static _Atomic uint32_t kh_dispatch_classes_registered;

static void
kh_register_dispatch_classes(void)
{
	uint32_t expected = 0;
	if (!atomic_compare_exchange_strong_explicit(
			&kh_dispatch_classes_registered, &expected, 1,
			memory_order_acq_rel, memory_order_acquire)) {
		return;
	}
	void *nsobject = objc_getClass("NSObject");
	(void)kh_objc_class_register((const uint8_t *)"OS_dispatch_queue",
			nsobject, &kh_dispatch_queue_class, &kh_dispatch_queue_metaclass,
			0, 1);
}

KH_EXPORT void kh_product_init(void);
KH_EXPORT void
kh_product_init(void)
{
	kh_register_dispatch_classes();
}

KH_EXPORT void *kh_swift_os_dispatch_queue_metadata(void)
		__asm("_$sSo17OS_dispatch_queueCMa");
KH_EXPORT void *
kh_swift_os_dispatch_queue_metadata(void)
{
	kh_register_dispatch_classes();
	return &kh_dispatch_queue_class;
}

KH_EXPORT intptr_t kh_swift_dispatch_semaphore_signal(dispatch_semaphore_t dsema)
		__asm("_$sSo21OS_dispatch_semaphoreC8DispatchE6signalSiyF");
KH_EXPORT intptr_t
kh_swift_dispatch_semaphore_signal(dispatch_semaphore_t dsema)
{
	return dispatch_semaphore_signal(dsema);
}

KH_EXPORT void kh_swift_dispatch_semaphore_wait(dispatch_semaphore_t dsema)
		__asm("_$sSo21OS_dispatch_semaphoreC8DispatchE4waityyF");
KH_EXPORT void
kh_swift_dispatch_semaphore_wait(dispatch_semaphore_t dsema)
{
	(void)dispatch_semaphore_wait(dsema, KH_DISPATCH_TIME_FOREVER);
}

KH_EXPORT dispatch_queue_t dispatch_get_main_queue(void);
KH_EXPORT dispatch_queue_t
dispatch_get_main_queue(void)
{
	return &_dispatch_main_q;
}

KH_EXPORT void kh_dispatch_assert_queue_v2(dispatch_queue_t queue)
		__asm("_dispatch_assert_queue$V2");
KH_EXPORT void
kh_dispatch_assert_queue_v2(dispatch_queue_t queue)
{
	dispatch_assert_queue(queue);
}

KH_EXPORT qos_class_t qos_class_self(void);
KH_EXPORT qos_class_t
qos_class_self(void)
{
	return KH_QOS_CLASS_DEFAULT;
}
