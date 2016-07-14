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

// dispatch/queue.h

import CDispatch

public struct DispatchQueueAttributes : OptionSet {
	public let rawValue: UInt64
	public init(rawValue: UInt64) { self.rawValue = rawValue }

	public static let serial = DispatchQueueAttributes(rawValue: 0<<0)
	public static let concurrent = DispatchQueueAttributes(rawValue: 1<<1)

	@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
	public static let initiallyInactive = DispatchQueueAttributes(rawValue: 1<<2)

	@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
	public static let autoreleaseInherit = DispatchQueueAttributes(rawValue: 1<<3)

	@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
	public static let autoreleaseWorkItem = DispatchQueueAttributes(rawValue: 1<<4)

	@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
	public static let autoreleaseNever = DispatchQueueAttributes(rawValue: 1<<5)

	@available(OSX 10.10, iOS 8.0, *)
	public static let qosUserInteractive = DispatchQueueAttributes(rawValue: 1<<6)

	@available(OSX 10.10, iOS 8.0, *)
	public static let qosUserInitiated = DispatchQueueAttributes(rawValue: 1<<7)

	@available(OSX 10.10, iOS 8.0, *)
	public static let qosDefault = DispatchQueueAttributes(rawValue: 1<<8)

	@available(OSX 10.10, iOS 8.0, *)
	public static let qosUtility = DispatchQueueAttributes(rawValue: 1<<9)

	@available(OSX 10.10, iOS 8.0, *)
	public static let qosBackground = DispatchQueueAttributes(rawValue: 1<<10)

	@available(*, deprecated, message: ".noQoS has no effect, it should not be used")
	public static let noQoS = DispatchQueueAttributes(rawValue: 1<<11)

	private var attr: dispatch_queue_attr_t? {
		var attr: dispatch_queue_attr_t?

		if self.contains(.concurrent) {
			attr = _swift_dispatch_queue_concurrent()
		}
		if #available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *) {
			if self.contains(.initiallyInactive) {
				attr = CDispatch.dispatch_queue_attr_make_initially_inactive(attr)
			}
			if self.contains(.autoreleaseWorkItem) {
				// DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM
				attr = CDispatch.dispatch_queue_attr_make_with_autorelease_frequency(attr, dispatch_autorelease_frequency_t(1))
			} else if self.contains(.autoreleaseInherit) {
				// DISPATCH_AUTORELEASE_FREQUENCY_INHERIT
				attr = CDispatch.dispatch_queue_attr_make_with_autorelease_frequency(attr, dispatch_autorelease_frequency_t(0))
			} else if self.contains(.autoreleaseNever) {
				// DISPATCH_AUTORELEASE_FREQUENCY_NEVER
				attr = CDispatch.dispatch_queue_attr_make_with_autorelease_frequency(attr, dispatch_autorelease_frequency_t(2))
			}
		}
		if #available(OSX 10.10, iOS 8.0, *) {
			if self.contains(.qosUserInteractive) {
				attr = CDispatch.dispatch_queue_attr_make_with_qos_class(attr, _OSQoSClass.QOS_CLASS_USER_INTERACTIVE.rawValue, 0)
			} else if self.contains(.qosUserInitiated) {
				attr = CDispatch.dispatch_queue_attr_make_with_qos_class(attr, _OSQoSClass.QOS_CLASS_USER_INITIATED.rawValue, 0)
			} else if self.contains(.qosDefault) {
				attr = CDispatch.dispatch_queue_attr_make_with_qos_class(attr, _OSQoSClass.QOS_CLASS_DEFAULT.rawValue, 0)
			} else if self.contains(.qosUtility) {
				attr = CDispatch.dispatch_queue_attr_make_with_qos_class(attr, _OSQoSClass.QOS_CLASS_UTILITY.rawValue, 0)
			} else if self.contains(.qosBackground) {
				attr = CDispatch.dispatch_queue_attr_make_with_qos_class(attr, _OSQoSClass.QOS_CLASS_BACKGROUND.rawValue, 0)
			}
		}
		return attr
	}
}


public final class DispatchSpecificKey<T> {
	public init() {}
}

internal class _DispatchSpecificValue<T> {
	internal let value: T
	internal init(value: T) { self.value = value }
}

public extension DispatchQueue {

	public struct GlobalAttributes : OptionSet {
		public let rawValue: UInt64
		public init(rawValue: UInt64) { self.rawValue = rawValue }

		@available(OSX 10.10, iOS 8.0, *)
		public static let qosUserInteractive = GlobalAttributes(rawValue: 1<<0)

		@available(OSX 10.10, iOS 8.0, *)
		public static let qosUserInitiated = GlobalAttributes(rawValue: 1<<1)

		@available(OSX 10.10, iOS 8.0, *)
		public static let qosDefault = GlobalAttributes(rawValue: 1<<2)

		@available(OSX 10.10, iOS 8.0, *)
		public static let qosUtility = GlobalAttributes(rawValue: 1<<3)

		@available(OSX 10.10, iOS 8.0, *)
		public static let qosBackground = GlobalAttributes(rawValue: 1<<4)

		// Avoid using our own deprecated constants here by declaring
		// non-deprecated constants and then basing the public ones on those.
		internal static let _priorityHigh = GlobalAttributes(rawValue: 1<<5)
		internal static let _priorityDefault = GlobalAttributes(rawValue: 1<<6)
		internal static let _priorityLow = GlobalAttributes(rawValue: 1<<7)
		internal static let _priorityBackground = GlobalAttributes(rawValue: 1<<8)

		@available(OSX, deprecated: 10.10, message: "Use qos attributes instead")
		@available(*, deprecated: 8.0, message: "Use qos attributes instead")
		public static let priorityHigh = _priorityHigh

		@available(OSX, deprecated: 10.10, message: "Use qos attributes instead")
		@available(*, deprecated: 8.0, message: "Use qos attributes instead")
		public static let priorityDefault = _priorityDefault

		@available(OSX, deprecated: 10.10, message: "Use qos attributes instead")
		@available(*, deprecated: 8.0, message: "Use qos attributes instead")
		public static let priorityLow = _priorityLow

		@available(OSX, deprecated: 10.10, message: "Use qos attributes instead")
		@available(*, deprecated: 8.0, message: "Use qos attributes instead")
		public static let priorityBackground = _priorityBackground

		internal var _translatedValue: Int {
			if #available(OSX 10.10, iOS 8.0, *) {
				if self.contains(.qosUserInteractive) { return Int(_OSQoSClass.QOS_CLASS_USER_INTERACTIVE.rawValue) }
				else if self.contains(.qosUserInitiated) { return Int(_OSQoSClass.QOS_CLASS_USER_INITIATED.rawValue) }
				else if self.contains(.qosDefault) { return Int(_OSQoSClass.QOS_CLASS_DEFAULT.rawValue) }
				else if self.contains(.qosUtility) { return Int(_OSQoSClass.QOS_CLASS_UTILITY.rawValue) }
				else { return Int(_OSQoSClass.QOS_CLASS_BACKGROUND.rawValue) }
			}
			if self.contains(._priorityHigh) { return 2 } // DISPATCH_QUEUE_PRIORITY_HIGH
			else if self.contains(._priorityDefault) { return 0 } // DISPATCH_QUEUE_PRIORITY_DEFAULT
			else if self.contains(._priorityLow) { return -2 } // // DISPATCH_QUEUE_PRIORITY_LOW
			else if self.contains(._priorityBackground) { return Int(Int16.min) } // // DISPATCH_QUEUE_PRIORITY_BACKGROUND
			return 0
		}
	}

	public class func concurrentPerform(iterations: Int, execute work: @noescape (Int) -> Void) {
		_swift_dispatch_apply_current(iterations, work)
	}

	public class var main: DispatchQueue {
		return DispatchQueue(queue: _swift_dispatch_get_main_queue())
	}

	public class func global(attributes: GlobalAttributes = []) -> DispatchQueue {
		// SubOptimal?  Should we be caching these global DispatchQueue objects?
		return DispatchQueue(queue:dispatch_get_global_queue(attributes._translatedValue, 0))
	}

	public class func getSpecific<T>(key: DispatchSpecificKey<T>) -> T? {
		let k = Unmanaged.passUnretained(key).toOpaque()
		if let p = CDispatch.dispatch_get_specific(k) {
			let v = Unmanaged<_DispatchSpecificValue<T>>
				.fromOpaque(p)
				.takeUnretainedValue()
			return v.value
		}
		return nil
	}

	public convenience init(
		label: String,
		attributes: DispatchQueueAttributes = .serial, 
		target: DispatchQueue? = nil)
	{
		if #available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *) {
			self.init(__label: label, attr: attributes.attr, queue: target)
		} else {
			self.init(__label: label, attr: attributes.attr)
			if let tq = target { self.setTarget(queue: tq) }
		}
	}

	public var label: String {
		return String(validatingUTF8: dispatch_queue_get_label(self.__wrapped))!
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func sync(execute workItem: DispatchWorkItem) {
		dispatch_sync(self.__wrapped, workItem._block)
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func async(execute workItem: DispatchWorkItem) {
		// _swift_dispatch_{group,}_async preserves the @convention(block) 
		// for work item blocks.
		if let g = workItem._group {
			dispatch_group_async(g.__wrapped, self.__wrapped, workItem._block)
		} else {
			dispatch_async(self.__wrapped, workItem._block)
		}
	}

	public func async(group: DispatchGroup? = nil, qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], execute work: @convention(block) () -> Void) {
		if group == nil && qos == .unspecified && flags.isEmpty {
			// Fast-path route for the most common API usage
			dispatch_async(self.__wrapped, work)
			return
		}

		if #available(OSX 10.10, iOS 8.0, *), (qos != .unspecified || !flags.isEmpty) {
			let workItem = DispatchWorkItem(qos: qos, flags: flags, block: work)
			if let g = group {
				dispatch_group_async(g.__wrapped, self.__wrapped, workItem._block)
			} else {
				dispatch_async(self.__wrapped, workItem._block)
			}
		} else {
			if let g = group {
				dispatch_group_async(g.__wrapped, self.__wrapped, work)
			} else {
				dispatch_async(self.__wrapped, work)
			}
		}
	}

	private func _syncBarrier(block: @noescape () -> ()) {
		dispatch_barrier_sync(self.__wrapped, block)
	}

	private func _syncHelper<T>(
		fn: (@noescape () -> ()) -> (), 
		execute work: @noescape () throws -> T, 
		rescue: ((Swift.Error) throws -> (T))) rethrows -> T
	{
		var result: T?
		var error: Swift.Error?
		fn {
			do {
				result = try work()
			} catch let e {
				error = e
			}
		}
		if let e = error {
			return try rescue(e)
		} else {
			return result!
		}
	}

	@available(OSX 10.10, iOS 8.0, *)
	private func _syncHelper<T>(
		fn: (DispatchWorkItem) -> (), 
		flags: DispatchWorkItemFlags,
		execute work: @noescape () throws -> T,
		rescue: ((Swift.Error) throws -> (T))) rethrows -> T
	{
		var result: T?
		var error: Swift.Error?
		let workItem = DispatchWorkItem(flags: flags, noescapeBlock: {
			do {
				result = try work()
			} catch let e {
				error = e 
			}
		})
		fn(workItem)
		if let e = error {
			return try rescue(e)
		} else {
			return result!
		}
	}

	public func sync<T>(execute work: @noescape () throws -> T) rethrows -> T {
		return try self._syncHelper(fn: sync, execute: work, rescue: { throw $0 })
	}

	public func sync<T>(flags: DispatchWorkItemFlags, execute work: @noescape () throws -> T) rethrows -> T {
		if flags == .barrier {
			return try self._syncHelper(fn: _syncBarrier, execute: work, rescue: { throw $0 })
		} else if #available(OSX 10.10, iOS 8.0, *), !flags.isEmpty {
			return try self._syncHelper(fn: sync, flags: flags, execute: work, rescue: { throw $0 })
		} else {
			return try self._syncHelper(fn: sync, execute: work, rescue: { throw $0 })
		}
	}

	public func after(when: DispatchTime, qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], execute work: @convention(block) () -> Void) {
		if #available(OSX 10.10, iOS 8.0, *), qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: work)
			dispatch_after(when.rawValue, self.__wrapped, item._block)
		} else {
			dispatch_after(when.rawValue, self.__wrapped, work)
		}
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func after(when: DispatchTime, execute: DispatchWorkItem) {
		dispatch_after(when.rawValue, self.__wrapped, execute._block)
	}

	public func after(walltime when: DispatchWallTime, qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], execute work: @convention(block) () -> Void) {
		if #available(OSX 10.10, iOS 8.0, *), qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: work)
			dispatch_after(when.rawValue, self.__wrapped, item._block)
		} else {
			dispatch_after(when.rawValue, self.__wrapped, work)
		}
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func after(walltime when: DispatchWallTime, execute: DispatchWorkItem) {
		dispatch_after(when.rawValue, self.__wrapped, execute._block)
	}

	@available(OSX 10.10, iOS 8.0, *)
	public var qos: DispatchQoS {
		var relPri: Int32 = 0
		let cls = DispatchQoS.QoSClass(qosClass: _OSQoSClass(qosClass: dispatch_queue_get_qos_class(self.__wrapped, &relPri))!)!
		return DispatchQoS(qosClass: cls, relativePriority: Int(relPri))
	}

	public func getSpecific<T>(key: DispatchSpecificKey<T>) -> T? {
		let k = Unmanaged.passUnretained(key).toOpaque()
		if let p = dispatch_queue_get_specific(self.__wrapped, k) {
			let v = Unmanaged<_DispatchSpecificValue<T>>
				.fromOpaque(p)
				.takeUnretainedValue()
			return v.value
		}
		return nil
	}

	public func setSpecific<T>(key: DispatchSpecificKey<T>, value: T) {
		let v = _DispatchSpecificValue(value: value)
		let k = Unmanaged.passUnretained(key).toOpaque()
		let p = Unmanaged.passRetained(v).toOpaque()
		dispatch_queue_set_specific(self.__wrapped, k, p, _destructDispatchSpecificValue)
	}
}

extension DispatchQueue {
	@available(*, deprecated, renamed: "DispatchQueue.sync(self:execute:)")
	public func synchronously(execute work: @noescape () -> ()) {
		sync(execute: work)
	}

	@available(OSX, introduced: 10.10, deprecated: 10.12, renamed: "DispatchQueue.sync(self:execute:)")
	@available(iOS, introduced: 8.0, deprecated: 10.0, renamed: "DispatchQueue.sync(self:execute:)")
	@available(*, deprecated, renamed: "DispatchQueue.sync(self:execute:)")
	public func synchronously(execute workItem: DispatchWorkItem) {
		sync(execute: workItem)
	}

	@available(OSX, introduced: 10.10, deprecated: 10.12, renamed: "DispatchQueue.async(self:execute:)")
	@available(iOS, introduced: 8.0, deprecated: 10.0, renamed: "DispatchQueue.async(self:execute:)")
	@available(*, deprecated, renamed: "DispatchQueue.async(self:execute:)")
	public func asynchronously(execute workItem: DispatchWorkItem) {
		async(execute: workItem)
	}

	@available(*, deprecated, renamed: "DispatchQueue.async(self:group:qos:flags:execute:)")
	public func asynchronously(group: DispatchGroup? = nil, qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], execute work: @convention(block) () -> Void) {
		async(group: group, qos: qos, flags: flags, execute: work)
	}

	@available(*, deprecated, renamed: "DispatchQueue.sync(self:execute:)")
	public func synchronously<T>(execute work: @noescape () throws -> T) rethrows -> T {
		return try sync(execute: work)
	}

	@available(*, deprecated, renamed: "DispatchQueue.sync(self:flags:execute:)")
	public func synchronously<T>(flags: DispatchWorkItemFlags, execute work: @noescape () throws -> T) rethrows -> T {
		return try sync(flags: flags, execute: work)
	}

	@available(*, deprecated, renamed: "DispatchQueue.concurrentPerform(iterations:execute:)")
	public func apply(applier iterations: Int, execute block: @noescape (Int) -> Void) {
		DispatchQueue.concurrentPerform(iterations: iterations, execute: block)
	}

	@available(*, deprecated, renamed: "DispatchQueue.setTarget(self:queue:)")
	public func setTargetQueue(queue: DispatchQueue) {
		self.setTarget(queue: queue)
	}
}

private func _destructDispatchSpecificValue(ptr: UnsafeMutablePointer<Void>?) {
	if let p = ptr {
		Unmanaged<AnyObject>.fromOpaque(p).release()
	}
}

@_silgen_name("_swift_dispatch_queue_concurrent")
internal func _swift_dispatch_queue_concurrent() -> dispatch_queue_attr_t

@_silgen_name("_swift_dispatch_get_main_queue")
internal func _swift_dispatch_get_main_queue() -> dispatch_queue_t

@_silgen_name("_swift_dispatch_apply_current_root_queue")
internal func _swift_dispatch_apply_current_root_queue() -> dispatch_queue_t

@_silgen_name("_swift_dispatch_apply_current")
internal func _swift_dispatch_apply_current(_ iterations: Int, _ block: @convention(block) @noescape (Int) -> Void)
