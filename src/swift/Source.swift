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

import CDispatch

public extension DispatchSourceProtocol {

	public func setEventHandler(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], handler: DispatchSourceHandler?) {
		if #available(OSX 10.10, iOS 8.0, *), let h = handler, qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: h)
			CDispatch.dispatch_source_set_event_handler((self as! DispatchSource).__wrapped, item._block)
		} else {
			CDispatch.dispatch_source_set_event_handler((self as! DispatchSource).__wrapped, handler)
		}
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func setEventHandler(handler: DispatchWorkItem) {
		CDispatch.dispatch_source_set_event_handler((self as! DispatchSource).__wrapped, handler._block)
	}

	public func setCancelHandler(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], handler: DispatchSourceHandler?) {
		if #available(OSX 10.10, iOS 8.0, *), let h = handler, qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: h)
			CDispatch.dispatch_source_set_cancel_handler((self as! DispatchSource).__wrapped, item._block)
		} else {
			CDispatch.dispatch_source_set_cancel_handler((self as! DispatchSource).__wrapped, handler)
		}
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func setCancelHandler(handler: DispatchWorkItem) {
		CDispatch.dispatch_source_set_cancel_handler((self as! DispatchSource).__wrapped, handler._block)
	}

	public func setRegistrationHandler(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], handler: DispatchSourceHandler?) {
		if #available(OSX 10.10, iOS 8.0, *), let h = handler, qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: h)
			CDispatch.dispatch_source_set_registration_handler((self as! DispatchSource).__wrapped, item._block)
		} else {
			CDispatch.dispatch_source_set_registration_handler((self as! DispatchSource).__wrapped, handler)
		}
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func setRegistrationHandler(handler: DispatchWorkItem) {
		CDispatch.dispatch_source_set_registration_handler((self as! DispatchSource).__wrapped, handler._block)
	}

	@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
	public func activate() {
		(self as! DispatchSource).activate()
	}

	public func cancel() {
		CDispatch.dispatch_source_cancel((self as! DispatchSource).__wrapped)
	}

	public func resume() {
		(self as! DispatchSource).resume()
	}

	public func suspend() {
		(self as! DispatchSource).suspend()
	}

	public var handle: UInt {
		return CDispatch.dispatch_source_get_handle((self as! DispatchSource).__wrapped)
	}

	public var mask: UInt {
		return CDispatch.dispatch_source_get_mask((self as! DispatchSource).__wrapped)
	}

	public var data: UInt {
		return CDispatch.dispatch_source_get_data((self as! DispatchSource).__wrapped)
	}

	public var isCancelled: Bool {
		return CDispatch.dispatch_source_testcancel((self as! DispatchSource).__wrapped) != 0
	}
}

public extension DispatchSource {
#if HAVE_MACH
	public struct MachSendEvent : OptionSet, RawRepresentable {
		public let rawValue: UInt
		public init(rawValue: UInt) { self.rawValue = rawValue }

		public static let dead = MachSendEvent(rawValue: 0x1)
	}
#endif

#if HAVE_MACH
	public struct MemoryPressureEvent : OptionSet, RawRepresentable {
		public let rawValue: UInt
		public init(rawValue: UInt) { self.rawValue = rawValue }

		public static let normal = MemoryPressureEvent(rawValue: 0x1)
		public static let warning = MemoryPressureEvent(rawValue: 0x2)
		public static let critical = MemoryPressureEvent(rawValue: 0x4)
		public static let all: MemoryPressureEvent = [.normal, .warning, .critical]
	}
#endif

#if !os(Linux)
	public struct ProcessEvent : OptionSet, RawRepresentable {
		public let rawValue: UInt
		public init(rawValue: UInt) { self.rawValue = rawValue }

		public static let exit = ProcessEvent(rawValue: 0x80000000)
		public static let fork = ProcessEvent(rawValue: 0x40000000)
		public static let exec = ProcessEvent(rawValue: 0x20000000)
		public static let signal = ProcessEvent(rawValue: 0x08000000)
		public static let all: ProcessEvent = [.exit, .fork, .exec, .signal]
	}
#endif

	public struct TimerFlags : OptionSet, RawRepresentable {
		public let rawValue: UInt
		public init(rawValue: UInt) { self.rawValue = rawValue }

		public static let strict = TimerFlags(rawValue: 1)
	}

	public struct FileSystemEvent : OptionSet, RawRepresentable {
		public let rawValue: UInt
		public init(rawValue: UInt) { self.rawValue = rawValue }

		public static let delete = FileSystemEvent(rawValue: 0x1)
		public static let write = FileSystemEvent(rawValue: 0x2)
		public static let extend = FileSystemEvent(rawValue: 0x4)
		public static let attrib = FileSystemEvent(rawValue: 0x8)
		public static let link = FileSystemEvent(rawValue: 0x10)
		public static let rename = FileSystemEvent(rawValue: 0x20)
		public static let revoke = FileSystemEvent(rawValue: 0x40)
		public static let funlock = FileSystemEvent(rawValue: 0x100)

		public static let all: FileSystemEvent = [
			.delete, .write, .extend, .attrib, .link, .rename, .revoke]
	}

#if HAVE_MACH
	public class func makeMachSendSource(port: mach_port_t, eventMask: MachSendEvent, queue: DispatchQueue? = nil) -> DispatchSourceMachSend {
		let source = dispatch_source_create(_swift_dispatch_source_type_mach_send(), UInt(port), eventMask.rawValue, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceMachSend
	}
#endif

#if HAVE_MACH
	public class func makeMachReceiveSource(port: mach_port_t, queue: DispatchQueue? = nil) -> DispatchSourceMachReceive {
		let source = dispatch_source_create(_swift_dispatch_source_type_mach_recv(), UInt(port), 0, queue?.__wrapped)
		return DispatchSource(source) as DispatchSourceMachReceive
	}
#endif

#if HAVE_MACH
	public class func makeMemoryPressureSource(eventMask: MemoryPressureEvent, queue: DispatchQueue? = nil) -> DispatchSourceMemoryPressure {
		let source = dispatch_source_create(_swift_dispatch_source_type_memorypressure(), 0, eventMask.rawValue, queue.__wrapped)
		return DispatchSourceMemoryPressure(source)
	}
#endif

#if !os(Linux)
	public class func makeProcessSource(identifier: pid_t, eventMask: ProcessEvent, queue: DispatchQueue? = nil) -> DispatchSourceProcess {
		let source = dispatch_source_create(_swift_dispatch_source_type_proc(), UInt(identifier), eventMask.rawValue, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceProcess
	}
#endif

	public class func makeReadSource(fileDescriptor: Int32, queue: DispatchQueue? = nil) -> DispatchSourceRead {
		let source = dispatch_source_create(_swift_dispatch_source_type_read(), UInt(fileDescriptor), 0, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceRead
	}

	public class func makeSignalSource(signal: Int32, queue: DispatchQueue? = nil) -> DispatchSourceSignal {
		let source = dispatch_source_create(_swift_dispatch_source_type_signal(), UInt(signal), 0, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceSignal
	}

	public class func makeTimerSource(flags: TimerFlags = [], queue: DispatchQueue? = nil) -> DispatchSourceTimer {
		let source = dispatch_source_create(_swift_dispatch_source_type_timer(), 0, flags.rawValue, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceTimer
	}

	public class func makeUserDataAddSource(queue: DispatchQueue? = nil) -> DispatchSourceUserDataAdd {
		let source = dispatch_source_create(_swift_dispatch_source_type_data_add(), 0, 0, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceUserDataAdd
	}

	public class func makeUserDataOrSource(queue: DispatchQueue? = nil) -> DispatchSourceUserDataOr {
		let source = dispatch_source_create(_swift_dispatch_source_type_data_or(), 0, 0, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceUserDataOr
	}

#if !os(Linux)
	public class func makeFileSystemObjectSource(fileDescriptor: Int32, eventMask: FileSystemEvent, queue: DispatchQueue? = nil) -> DispatchSourceFileSystemObject {
		let source = dispatch_source_create(_swift_dispatch_source_type_vnode(), UInt(fileDescriptor), eventMask.rawValue, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceFileSystemObject
	}
#endif

	public class func makeWriteSource(fileDescriptor: Int32, queue: DispatchQueue? = nil) -> DispatchSourceWrite {
		let source = dispatch_source_create(_swift_dispatch_source_type_write(), UInt(fileDescriptor), 0, queue?.__wrapped)
		return DispatchSource(source: source) as DispatchSourceWrite
	}
}

#if HAVE_MACH
public extension DispatchSourceMachSend {
	public var handle: mach_port_t {
		return mach_port_t(dispatch_source_get_handle(self as! DispatchSource))
	}

	public var data: DispatchSource.MachSendEvent {
		let data = dispatch_source_get_data(self as! DispatchSource)
		return DispatchSource.MachSendEvent(rawValue: data)
	}

	public var mask: DispatchSource.MachSendEvent {
		let mask = dispatch_source_get_mask(self as! DispatchSource)
		return DispatchSource.MachSendEvent(rawValue: mask)
	}
}
#endif

#if HAVE_MACH
public extension DispatchSourceMachReceive {
	public var handle: mach_port_t {
		return mach_port_t(dispatch_source_get_handle(self as! DispatchSource))
	}
}
#endif

#if HAVE_MACH
public extension DispatchSourceMemoryPressure {
	public var data: DispatchSource.MemoryPressureEvent {
		let data = dispatch_source_get_data(self as! DispatchSource)
		return DispatchSource.MemoryPressureEvent(rawValue: data)
	}

	public var mask: DispatchSource.MemoryPressureEvent {
		let mask = dispatch_source_get_mask(self as! DispatchSource)
		return DispatchSource.MemoryPressureEvent(rawValue: mask)
	}
}
#endif

#if !os(Linux)
public extension DispatchSourceProcess {
	public var handle: pid_t {
		return pid_t(dispatch_source_get_handle(self as! DispatchSource))
	}

	public var data: DispatchSource.ProcessEvent {
		let data = dispatch_source_get_data(self as! DispatchSource)
		return DispatchSource.ProcessEvent(rawValue: data)
	}

	public var mask: DispatchSource.ProcessEvent {
		let mask = dispatch_source_get_mask(self as! DispatchSource)
		return DispatchSource.ProcessEvent(rawValue: mask)
	}
}
#endif

public extension DispatchSourceTimer {
	public func scheduleOneshot(deadline: DispatchTime, leeway: DispatchTimeInterval = .nanoseconds(0)) {
		dispatch_source_set_timer((self as! DispatchSource).__wrapped, deadline.rawValue, ~0, UInt64(leeway.rawValue))
	}

	public func scheduleOneshot(wallDeadline: DispatchWallTime, leeway: DispatchTimeInterval = .nanoseconds(0)) {
		dispatch_source_set_timer((self as! DispatchSource).__wrapped, wallDeadline.rawValue, ~0, UInt64(leeway.rawValue))
	}

	public func scheduleRepeating(deadline: DispatchTime, interval: DispatchTimeInterval, leeway: DispatchTimeInterval = .nanoseconds(0)) {
		dispatch_source_set_timer((self as! DispatchSource).__wrapped, deadline.rawValue, UInt64(interval.rawValue), UInt64(leeway.rawValue))
	}

	public func scheduleRepeating(deadline: DispatchTime, interval: Double, leeway: DispatchTimeInterval = .nanoseconds(0)) {
		dispatch_source_set_timer((self as! DispatchSource).__wrapped, deadline.rawValue, UInt64(interval * Double(NSEC_PER_SEC)), UInt64(leeway.rawValue))
	}

	public func scheduleRepeating(wallDeadline: DispatchWallTime, interval: DispatchTimeInterval, leeway: DispatchTimeInterval = .nanoseconds(0)) {
		dispatch_source_set_timer((self as! DispatchSource).__wrapped, wallDeadline.rawValue, UInt64(interval.rawValue), UInt64(leeway.rawValue))
	}

	public func scheduleRepeating(wallDeadline: DispatchWallTime, interval: Double, leeway: DispatchTimeInterval = .nanoseconds(0)) {
		dispatch_source_set_timer((self as! DispatchSource).__wrapped, wallDeadline.rawValue, UInt64(interval * Double(NSEC_PER_SEC)), UInt64(leeway.rawValue))
	}
}

#if !os(Linux)
public extension DispatchSourceFileSystemObject {
	public var handle: Int32 {
		return Int32(dispatch_source_get_handle((self as! DispatchSource).__wrapped))
	}

	public var data: DispatchSource.FileSystemEvent {
		let data = dispatch_source_get_data((self as! DispatchSource).__wrapped)
		return DispatchSource.FileSystemEvent(rawValue: data)
	}

	public var mask: DispatchSource.FileSystemEvent {
		let data = dispatch_source_get_mask((self as! DispatchSource).__wrapped)
		return DispatchSource.FileSystemEvent(rawValue: data)
	}
}
#endif

public extension DispatchSourceUserDataAdd {
	/// @function mergeData
	///
	/// @abstract
	/// Merges data into a dispatch source of type DISPATCH_SOURCE_TYPE_DATA_ADD or
	/// DISPATCH_SOURCE_TYPE_DATA_OR and submits its event handler block to its
	/// target queue.
	///
	/// @param value
	/// The value to coalesce with the pending data using a logical OR or an ADD
	/// as specified by the dispatch source type. A value of zero has no effect
	/// and will not result in the submission of the event handler block.
	public func add(data: UInt) {
		dispatch_source_merge_data((self as! DispatchSource).__wrapped, data)
	}
}

public extension DispatchSourceUserDataOr {
	/// @function mergeData
	///
	/// @abstract
	/// Merges data into a dispatch source of type DISPATCH_SOURCE_TYPE_DATA_ADD or
	/// DISPATCH_SOURCE_TYPE_DATA_OR and submits its event handler block to its
	/// target queue.
	///
	/// @param value
	/// The value to coalesce with the pending data using a logical OR or an ADD
	/// as specified by the dispatch source type. A value of zero has no effect
	/// and will not result in the submission of the event handler block.
	public func or(data: UInt) {
		dispatch_source_merge_data((self as! DispatchSource).__wrapped, data)
	}
}

@_silgen_name("_swift_dispatch_source_type_DATA_ADD")
internal func _swift_dispatch_source_type_data_add() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_DATA_OR")
internal func _swift_dispatch_source_type_data_or() -> dispatch_source_type_t

#if HAVE_MACH
@_silgen_name("_swift_dispatch_source_type_MACH_SEND")
internal func _swift_dispatch_source_type_mach_send() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_MACH_RECV")
internal func _swift_dispatch_source_type_mach_recv() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_MEMORYPRESSURE")
internal func _swift_dispatch_source_type_memorypressure() -> dispatch_source_type_t
#endif

#if !os(Linux)
@_silgen_name("_swift_dispatch_source_type_PROC")
internal func _swift_dispatch_source_type_proc() -> dispatch_source_type_t
#endif

@_silgen_name("_swift_dispatch_source_type_READ")
internal func _swift_dispatch_source_type_read() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_SIGNAL")
internal func _swift_dispatch_source_type_signal() -> dispatch_source_type_t

@_silgen_name("_swift_dispatch_source_type_TIMER")
internal func _swift_dispatch_source_type_timer() -> dispatch_source_type_t

#if !os(Linux)
@_silgen_name("_swift_dispatch_source_type_VNODE")
internal func _swift_dispatch_source_type_vnode() -> dispatch_source_type_t
#endif

@_silgen_name("_swift_dispatch_source_type_WRITE")
internal func _swift_dispatch_source_type_write() -> dispatch_source_type_t
