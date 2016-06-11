//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_exported import Dispatch
import CDispatch

//===----------------------------------------------------------------------===//
// Linux-specific overlay layer to compensate for lack of Objective-C
//
//   CDispatch classes and APIs are wrapped in Swift objects/functions
//   to achieve API compatibility with Darwin platforms and integration with
//   Swift retain/release operations.
//
//   Injecting an extra level of wrapping is sub-optimal, but pushing the Swift
//   object model down into the C libdispatch implementation is too invasive
//   a change to undertake at this stage of development.
//===----------------------------------------------------------------------===//

//////////
// Define Swift-level class hierarchy for subset of dispatch types
// returned from API methods with DISPATCH_RETURNS_RETAINED
//////////

public class dispatch_object_t {
  let cobj:OpaquePointer;

  deinit {
    CDispatch.dispatch_release(_to_dot(x: cobj))
  }

  init(_ cobj:OpaquePointer) {
    self.cobj = cobj;
  }
}

public class dispatch_data_t : dispatch_object_t {
}

public class dispatch_group_t : dispatch_object_t {
}

public class dispatch_io_t : dispatch_object_t {
}

public class dispatch_queue_t : dispatch_object_t {
}

public class dispatch_semaphore_t : dispatch_object_t {
}

public class dispatch_source_t : dispatch_object_t {
}

//////////
// base.h
//////////

public typealias dispatch_function_t = CDispatch.dispatch_function_t

//////////
// time.h 
//////////

public typealias dispatch_time_t = UInt64

public let NSEC_PER_SEC:UInt64 = 1000000000
public let NSEC_PER_MSEC:UInt64 = 1000000
public let USEC_PER_SEC:UInt64 = 1000000
public let NSEC_PER_USEC:UInt64 = 1000


public var DISPATCH_TIME_FOREVER:dispatch_time_t {
  return CDispatch.DISPATCH_TIME_FOREVER
}

public var DISPATCH_TIME_NOW:dispatch_time_t {
  return CDispatch.DISPATCH_TIME_NOW
}

public func dispatch_time(_ when:dispatch_time_t, _ delta:Int64) -> dispatch_time_t {
  return CDispatch.dispatch_time(when, delta)
}

public func dispatch_walltime(_ when:UnsafePointer<timespec>, _ delta:Int64) -> dispatch_time_t {
  return CDispatch.dispatch_walltime(when, delta)
}

//////////
// object.h
//////////

// dispatch_retain/dispatch_release intentionally not exposed.

public func dispatch_get_context(_ object:dispatch_object_t) -> UnsafeMutablePointer<Void> {
  return CDispatch.dispatch_get_context(_to_dot(x: object.cobj))
}

public func dispatch_set_context(_ object:dispatch_object_t, _ context:UnsafeMutablePointer<Void>) -> Void {
  CDispatch.dispatch_set_context(_to_dot(x: object.cobj), context)
}

public func dispatch_set_finalizer_f(_ object:dispatch_object_t, 
                                     _ finalizer:dispatch_function_t) -> Void {
  CDispatch.dispatch_set_finalizer_f(_to_dot(x: object.cobj), finalizer)
}

public func dispatch_suspend(_ object:dispatch_object_t) -> Void {
  CDispatch.dispatch_suspend(_to_dot(x: object.cobj))
}  

public func dispatch_resume(_ object:dispatch_object_t) -> Void {
  CDispatch.dispatch_resume(_to_dot(x: object.cobj))
}  

//////////
// queue.h
//////////

public func dispatch_async(_ queue:dispatch_queue_t, _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_async(queue.cobj, block)
}

public func dispatch_sync(_ queue:dispatch_queue_t, _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_sync(queue.cobj, block)
}

public typealias dispatch_apply_block_t = @convention(block) (size_t) -> Void

public func dispatch_apply(_ iterations:Int, _ queue:dispatch_queue_t,
                           _ block:dispatch_apply_block_t) -> Void {
  CDispatch.dispatch_apply(iterations, queue.cobj, block)
}

// skip deprecated function dispatch_get_current_queue

public func dispatch_get_main_queue()-> dispatch_queue_t {
  return mainQueue;
}

public typealias dispatch_queue_priority_t = CDispatch.dispatch_queue_priority_t

public var DISPATCH_QUEUE_PRIORITY_HIGH: dispatch_queue_priority_t {
  return 2
}
public var DISPATCH_QUEUE_PRIORITY_DEFAULT: dispatch_queue_priority_t {
  return 0
}
public var DISPATCH_QUEUE_PRIORITY_LOW: dispatch_queue_priority_t {
  return -2
}
public var DISPATCH_QUEUE_PRIORITY_BACKGROUND: dispatch_queue_priority_t {
  return -32768
}

public func dispatch_get_global_queue(_ identifier:Int, _ flags:UInt) -> dispatch_queue_t {
  return dispatch_queue_t(CDispatch.dispatch_get_global_queue(identifier, flags))
}

// skip dispatch_queue_attr_make_with_qos_class; no QoS on Linux

public typealias dispatch_queue_attr_t = CDispatch.dispatch_queue_attr_t

public var DISPATCH_QUEUE_SERIAL: dispatch_queue_attr_t! {
  return nil
}
public var DISPATCH_QUEUE_CONCURRENT: dispatch_queue_attr_t {
  return _swift_dispatch_queue_concurrent()
}

public func dispatch_queue_create(_ label:UnsafePointer<Int8>,
                                  _ attr:dispatch_queue_attr_t?) -> dispatch_queue_t {
  return dispatch_queue_t(CDispatch.dispatch_queue_create(label, attr))
}

public var DISPATCH_CURRENT_QUEUE_LABEL: dispatch_queue_t? {
  return nil
}

public func dispatch_queue_get_label(_ queue:dispatch_queue_t?) -> UnsafePointer<Int8> {
  return CDispatch.dispatch_queue_get_label(queue != nil ? queue!.cobj : nil)
}

// skip dispatch_queue_get_qos_class; no QoS on Linux

public var DISPATCH_TARGET_QUEUE_DEFAULT: dispatch_queue_t? {
  return nil
}

public func dispatch_set_target_queue(_ object:dispatch_object_t, queue:dispatch_queue_t?) -> Void {
  CDispatch.dispatch_set_target_queue(_to_dot(x: object.cobj), queue != nil ? queue!.cobj : nil)
}

public func dispatch_main() -> Void {
  CDispatch.dispatch_main()
}

public func dispatch_after(_ when:dispatch_time_t, _ queue:dispatch_queue_t,
                           _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_after(when, queue.cobj, block)
}

public func dispatch_barrier_async(_ queue:dispatch_queue_t, _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_barrier_async(queue.cobj, block)
}

public func dispatch_barrier_sync(_ queue:dispatch_queue_t, _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_barrier_sync(queue.cobj, block)
}

public func dispatch_queue_set_specific(_ queue:dispatch_queue_t,
                                        _ key:UnsafePointer<Void>,
                                        _ context:UnsafeMutablePointer<Void>,
                                        _ destructor:dispatch_function_t?) -> Void {
  CDispatch.dispatch_queue_set_specific(queue.cobj, key, context, destructor)
}

public func dispatch_queue_get_specific(_ queue:dispatch_queue_t,
                                        _ key:UnsafePointer<Void>) -> UnsafeMutablePointer<Void>? {
  return CDispatch.dispatch_queue_get_specific(queue.cobj, key)
}

public func dispatch_get_specific(_ key:UnsafePointer<Void>) -> UnsafeMutablePointer<Void>? {
  return CDispatch.dispatch_get_specific(key)
}


//////////
// block.h
//////////

/// The type of blocks submitted to dispatch queues, which take no arguments
/// and have no return value.
///
/// The dispatch_block_t typealias is different from usual closures in that it
/// uses @convention(block). This is to avoid unnecessary bridging between
/// C blocks and Swift closures, which interferes with Grand Central Dispatch
/// APIs that depend on the referential identity of a block.
public typealias dispatch_block_t = @convention(block) () -> Void

public typealias dispatch_block_flags_t = UInt

public var DISPATCH_BLOCK_BARRIER:dispatch_block_flags_t {
  return 0x1
}
public var DISPATCH_BLOCK_DETACHED:dispatch_block_flags_t { 
  return 0x2
}
public var DISPATCH_BLOCK_ASSIGN_CURRENT:dispatch_block_flags_t {
  return 0x4
}
public var DISPATCH_BLOCK_NO_QOS_CLASS:dispatch_block_flags_t {
  return 0x8
}
public var DISPATCH_BLOCK_INHERIT_QOS_CLASS:dispatch_block_flags_t {
  return 0x10
}  
public var DISPATCH_BLOCK_ENFORCE_QOS_CLASS:dispatch_block_flags_t {
  return 0x20
}  

public func dispatch_block_create(_ flags:dispatch_block_flags_t,
                                  _ block:dispatch_block_t) -> dispatch_block_t? {
  return CDispatch.dispatch_block_create(flags, block)
}  

// skip dispatch_block_create_with_qos_class; no QoS on Linux
        
public func dispatch_block_perform(_ flags:dispatch_block_flags_t, _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_block_perform(flags, block)
}

public func dispatch_block_wait(_ block:dispatch_block_t, _ timeout:dispatch_time_t) -> Int {
  return CDispatch.dispatch_block_wait(block, timeout)
}

public func dispatch_block_notify(_ block:dispatch_block_t, _ queue:dispatch_queue_t,
                                  _ notification_block:dispatch_block_t) -> Void {
  return CDispatch.dispatch_block_notify(block, queue.cobj, notification_block)        
}

public func dispatch_block_cancel(_ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_block_cancel(block)
}

public func dispatch_block_testcancel(_ block:dispatch_block_t) -> Int {
  return CDispatch.dispatch_block_testcancel(block)
}

//////////
// source.h
//////////

public typealias dispatch_source_type_t = OpaquePointer

public var DISPATCH_SOURCE_TYPE_DATA_ADD: dispatch_source_type_t {
  return _swift_dispatch_source_type_data_add()
}
public var DISPATCH_SOURCE_TYPE_DATA_OR: dispatch_source_type_t {
  return _swift_dispatch_source_type_data_or()
}
public var DISPATCH_SOURCE_TYPE_READ: dispatch_source_type_t {
  return _swift_dispatch_source_type_read()
}
public var DISPATCH_SOURCE_TYPE_PROC: dispatch_source_type_t {
  return _swift_dispatch_source_type_proc()
}
public var DISPATCH_SOURCE_TYPE_SIGNAL: dispatch_source_type_t {
  return _swift_dispatch_source_type_signal()
}
public var DISPATCH_SOURCE_TYPE_TIMER: dispatch_source_type_t {
  return _swift_dispatch_source_type_timer()
}
public var DISPATCH_SOURCE_TYPE_VNODE: dispatch_source_type_t {
  return _swift_dispatch_source_type_vnode()
}
public var DISPATCH_SOURCE_TYPE_WRITE: dispatch_source_type_t {
  return _swift_dispatch_source_type_write()
}

public typealias dispatch_source_proc_flags_t = CDispatch.dispatch_source_proc_flags_t
public var DISPATCH_PROC_EXIT: dispatch_source_proc_flags_t { return 0x80000000 }
public var DISPATCH_PROC_FORK: dispatch_source_proc_flags_t { return 0x40000000 }
public var DISPATCH_PROC_EXEC: dispatch_source_proc_flags_t { return 0x20000000 }
public var DISPATCH_PROC_SIGNAL: dispatch_source_proc_flags_t { return 0x08000000 }

public typealias dispatch_source_vnode_flags_t = CDispatch.dispatch_source_vnode_flags_t
public var DISPATCH_VNODE_DELETE: dispatch_source_vnode_flags_t { return 0x1 }
public var DISPATCH_VNODE_WRITE:  dispatch_source_vnode_flags_t { return 0x2 }
public var DISPATCH_VNODE_EXTEND: dispatch_source_vnode_flags_t { return 0x4 }
public var DISPATCH_VNODE_ATTRIB: dispatch_source_vnode_flags_t { return 0x8 }
public var DISPATCH_VNODE_LINK:   dispatch_source_vnode_flags_t { return 0x10 }
public var DISPATCH_VNODE_RENAME: dispatch_source_vnode_flags_t { return 0x20 }
public var DISPATCH_VNODE_REVOKE: dispatch_source_vnode_flags_t { return 0x40 }

public typealias dispatch_source_timer_flags_t = CDispatch.dispatch_source_timer_flags_t
public var DISPATCH_TIMER_STRICT: dispatch_source_timer_flags_t { return 1 }

public func dispatch_source_create(_ type:dispatch_source_type_t,
                                   _ handle:UInt,
                                   _ mask:UInt,
                                   _ queue:dispatch_queue_t?) -> dispatch_source_t {
  let cqueue: OpaquePointer? = queue != nil ? queue!.cobj : nil
  return dispatch_source_t(CDispatch.dispatch_source_create(type, handle, mask, cqueue))
}

public func dispatch_source_set_event_handler(_ source:dispatch_source_t,
                                              _ handler:dispatch_block_t) -> Void {
  CDispatch.dispatch_source_set_event_handler(source.cobj, handler)
}

public func dispatch_source_set_cancel_handler(_ source:dispatch_source_t,
                                               _ handler:dispatch_block_t) -> Void {
  CDispatch.dispatch_source_set_cancel_handler(source.cobj, handler)
}

public func dispatch_source_cancel(_ source:dispatch_source_t) -> Void {
  CDispatch.dispatch_source_cancel(source.cobj)
}

public func dispatch_source_testcancel(_ source:dispatch_source_t) -> Int {
  return CDispatch.dispatch_source_testcancel(source.cobj)
}

public func dispatch_source_get_handle(_ source:dispatch_source_t) -> UInt {
  return CDispatch.dispatch_source_get_handle(source.cobj)
}

public func dispatch_source_get_mask(_ source:dispatch_source_t) -> UInt {
  return CDispatch.dispatch_source_get_mask(source.cobj)
}

public func dispatch_source_get_data(_ source:dispatch_source_t) -> UInt {
  return CDispatch.dispatch_source_get_data(source.cobj)
}

public func dispatch_source_merge_data(_ source:dispatch_source_t,_ value:UInt) -> Void {
  CDispatch.dispatch_source_merge_data(source.cobj, value)
}

public func dispatch_source_set_timer(_ source:dispatch_source_t,
                                      _ start:dispatch_time_t,
                                      _ interval:UInt64,
                                      _ leeway:UInt64) -> Void {
  CDispatch.dispatch_source_set_timer(source.cobj, start, interval, leeway)
}

public func dispatch_source_set_registration_handler(_ source:dispatch_source_t,
                                                     _ handler:dispatch_block_t) -> Void {
  CDispatch.dispatch_source_set_registration_handler(source.cobj, handler)
}

//////////
// group.h
//////////

public func dispatch_group_create() -> dispatch_group_t {
  return dispatch_group_t(CDispatch.dispatch_group_create())
}

public func dispatch_group_async(_ group:dispatch_group_t, _ queue:dispatch_queue_t,
                                 _ block:dispatch_block_t?) -> Void {
  CDispatch.dispatch_group_async(group.cobj, queue.cobj, block!)
}

@discardableResult
public func dispatch_group_wait(_ group:dispatch_group_t, _ timeout:dispatch_time_t) -> Int {
  return CDispatch.dispatch_group_wait(group.cobj, timeout)
}

public func dispatch_group_notify(_ group:dispatch_group_t, _ queue:dispatch_queue_t,
                                  _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_group_notify(group.cobj, queue.cobj, block)
}

public func dispatch_group_enter(_ group:dispatch_group_t) -> Void {
  CDispatch.dispatch_group_enter(group.cobj)
}

public func dispatch_group_leave(_ group:dispatch_group_t) -> Void {
  CDispatch.dispatch_group_leave(group.cobj)
}

//////////
// semaphore.h
//////////

public func dispatch_semaphore_create(_ value:Int) -> dispatch_semaphore_t {
  return dispatch_semaphore_t(CDispatch.dispatch_semaphore_create(value))
}

@discardableResult
public func dispatch_semaphore_wait(_ dsema:dispatch_semaphore_t, _ timeout:dispatch_time_t) -> Int {
  return CDispatch.dispatch_semaphore_wait(dsema.cobj, timeout)
}

@discardableResult
public func dispatch_semaphore_signal(_ dsema:dispatch_semaphore_t) -> Int {
  return CDispatch.dispatch_semaphore_signal(dsema.cobj)
}

//////////
// once.h
//////////

public func dispatch_once(_ predicate:UnsafeMutablePointer<Int>, _ block:dispatch_block_t) -> Void {
  CDispatch.dispatch_once(predicate, block)
}

//////////
// data.h
//////////

public var dispatch_data_empty: dispatch_data_t {
  return _swift_dispatch_data_empty()
}

public func dispatch_data_create(_ buffer:UnsafePointer<Void>?, _ size:size_t,
                                 _ queue:dispatch_queue_t?,    _ destructor:dispatch_block_t?) -> dispatch_data_t {
  return dispatch_data_t(CDispatch.dispatch_data_create(buffer, size, queue != nil ? queue!.cobj : nil, destructor))
}

public func dispatch_data_get_size(_ data:dispatch_data_t)-> size_t {
  return CDispatch.dispatch_data_get_size(data.cobj)
}

public func dispatch_data_create_map(_ data:dispatch_data_t,
                                     _ buffer_ptr:UnsafeMutablePointer<UnsafePointer<Void>?>,
                                     _ size_ptr:UnsafeMutablePointer<size_t>) -> dispatch_data_t {
  return dispatch_data_t(CDispatch.dispatch_data_create_map(data.cobj, buffer_ptr, size_ptr))
}

public func dispatch_data_create_concat(_ data1:dispatch_data_t, _ data2:dispatch_data_t)->dispatch_data_t {
  return dispatch_data_t(CDispatch.dispatch_data_create_concat(data1.cobj, data2.cobj))
}

public func dispatch_data_create_subrange(_ data:dispatch_data_t,
                                          _ offset:size_t, _ length:size_t) -> dispatch_data_t {
  return dispatch_data_t(CDispatch.dispatch_data_create_subrange(data.cobj, offset, length))
}

// TODO: defer dispatch_data_applier_t/dispatch_data_apply
//       a little unclear how best to unwrap/wrap within the block.

public func dispatch_data_apply(_ data: dispatch_data_t, _ applier: dispatch_data_applier_t) -> Bool {
    return CDispatch.dispatch_data_apply(data.cobj, applier)
}

public func dispatch_data_copy_region(_ data:dispatch_data_t, _ location:size_t,
                                      _ offset_ptr:UnsafeMutablePointer<size_t>) -> dispatch_data_t {
  return dispatch_data_t(CDispatch.dispatch_data_copy_region(data.cobj, location, offset_ptr))
}

//////////
// io.h
//////////

public typealias dispatch_fd_t = CDispatch.dispatch_fd_t

// TODO: defer dispatch_read (block with dispatch_data_t arg)

// TODO: defer dispatch_write (block with dispatch_data_t arg)

public var DISPATCH_IO_STREAM: dispatch_io_type_t {
  return 0
}
public var DISPATCH_IO_RANDOM: dispatch_io_type_t {
  return 1
}

public typealias dispatch_io_type_t = CDispatch.dispatch_io_type_t

public typealias dispatch_io_handler_block_t = @convention(block) (Int32) -> Void

public func dispatch_io_create(_ type:dispatch_io_type_t,
                               _ fd:dispatch_fd_t,
                               _ queue:dispatch_queue_t,
                               _ cleanup_handler:dispatch_io_handler_block_t) -> dispatch_io_t {
  return dispatch_io_t(CDispatch.dispatch_io_create(type, fd, queue.cobj, cleanup_handler))
}

public func dispatch_io_create_with_path(_ type:dispatch_io_type_t,
                                         _ path:UnsafePointer<Int8>,
                                         _ oflag:Int32, _ mode:mode_t,
                                         _ queue:dispatch_queue_t,
                                         _ cleanup_handler:dispatch_io_handler_block_t?) -> dispatch_io_t {
  return dispatch_io_t(CDispatch.dispatch_io_create_with_path(type, path, oflag, mode, queue.cobj, cleanup_handler))
}

public func dispatch_io_create_with_io(_ type:dispatch_io_type_t,
                                       _ io:dispatch_io_t,
                                       _ queue:dispatch_queue_t,
                                       _ cleanup_handler:dispatch_io_handler_block_t) -> dispatch_io_t {
  return dispatch_io_t(CDispatch.dispatch_io_create_with_io(type, io.cobj, queue.cobj, cleanup_handler))
}

public typealias dispatch_io_handler_t = (Bool, dispatch_data_t?, CInt) -> ()

public typealias io_handler = @convention(block) (Bool, CDispatch.dispatch_data_t?, CInt) -> () 

public func dispatch_io_read(_ channel: dispatch_io_t, 
                             _ offset: off_t,
                             _ length: size_t,
                             _ queue: dispatch_queue_t,
                             _ handler: dispatch_io_handler_t) -> Void {
  let wrapper_block = { (done: Bool, data: OpaquePointer?, errno: CInt) in
    if data != nil {
      CDispatch.dispatch_retain(_to_dot(x: data!))
      handler(done, dispatch_data_t(data!), errno)
    } else {
      handler(done, nil, errno)
    }    
  }
  CDispatch.dispatch_io_read(channel.cobj, offset, length, queue.cobj, wrapper_block)
}

public func dispatch_io_write(_ channel: dispatch_io_t,
                             _ offset: off_t,
                             _ data: dispatch_data_t,
                             _ queue: dispatch_queue_t,
                             _ handler: dispatch_io_handler_t) -> Void {
  let wrapper_block = { (done: Bool, data: OpaquePointer?, errno: CInt) in
    if data != nil {
      CDispatch.dispatch_retain(_to_dot(x: data!))
      handler(done, dispatch_data_t(data!), errno)
    } else {
      handler(done, nil, errno)
    }
  }
  CDispatch.dispatch_io_write(channel.cobj, offset, data.cobj, queue.cobj, wrapper_block)
}


public var DISPATCH_IO_STOP: dispatch_io_close_flags_t {
  return 1
}

public typealias dispatch_io_close_flags_t = CDispatch.dispatch_io_close_flags_t

public func dispatch_io_close(_ channel:dispatch_io_t, _ flags:dispatch_io_close_flags_t) -> Void {
  CDispatch.dispatch_io_close(channel.cobj, flags)
}

public func dispatch_io_barrier(_ channel:dispatch_io_t, _ barrier:dispatch_block_t) -> Void {
  CDispatch.dispatch_io_barrier(channel.cobj, barrier)
}

public func dispatch_io_get_descriptor(_ channel:dispatch_io_t) -> dispatch_fd_t {
  return CDispatch.dispatch_io_get_descriptor(channel.cobj)
}

public func dispatch_io_set_high_water(_ channel:dispatch_io_t, _ high_water:size_t) -> Void {
  CDispatch.dispatch_io_set_high_water(channel.cobj, high_water)
}

public func dispatch_io_set_low_water(_ channel:dispatch_io_t, _ low_water:size_t) -> Void {
  CDispatch.dispatch_io_set_low_water(channel.cobj, low_water)
}

public var DISPATCH_IO_STRICT_INTERVAL: dispatch_io_interval_flags_t {
  return 1
}

public typealias dispatch_io_interval_flags_t = CDispatch.dispatch_io_interval_flags_t

public func dispatch_io_set_interval(_ channel:dispatch_io_t,
                                     _ interval:UInt64,
                                     _ flags:dispatch_io_interval_flags_t) -> Void {
  CDispatch.dispatch_io_set_interval(channel.cobj, interval, flags)
}

//===----------------------------------------------------------------------===//
// Internal macros and helper functions.  Not part of the exported API
//===----------------------------------------------------------------------===//

internal let mainQueue:dispatch_queue_t = dispatch_queue_t(CDispatch.dispatch_get_main_queue())

@_silgen_name("_swift_dispatch_object_type_punner")
internal func _to_dot(x:OpaquePointer) -> CDispatch.dispatch_object_t

@_silgen_name("_swift_dispatch_queue_concurrent")
internal func _swift_dispatch_queue_concurrent() -> dispatch_queue_attr_t

@_silgen_name("_swift_dispatch_data_empty")
internal func _swift_dispatch_data_empty() -> dispatch_data_t

@_silgen_name("_swift_dispatch_source_type_DATA_ADD")
internal func _swift_dispatch_source_type_data_add() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_DATA_OR")
internal func _swift_dispatch_source_type_data_or() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_PROC")
internal func _swift_dispatch_source_type_proc() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_READ")
internal func _swift_dispatch_source_type_read() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_SIGNAL")
internal func _swift_dispatch_source_type_signal() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_TIMER")
internal func _swift_dispatch_source_type_timer() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_VNODE")
internal func _swift_dispatch_source_type_vnode() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_WRITE")
internal func _swift_dispatch_source_type_write() -> dispatch_source_type_t

//===----------------------------------------------------------------------===//
// Macros
// FIXME: rdar://16851050 update API so these import better
//===----------------------------------------------------------------------===//

// dispatch/io.h


// dispatch/source.h
// FIXME: DISPATCH_SOURCE_TYPE_*
