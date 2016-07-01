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

/// dispatch_assert

@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
public enum DispatchPredicate {
	case onQueue(DispatchQueue)
	case onQueueAsBarrier(DispatchQueue)
	case notOnQueue(DispatchQueue)
}

@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
public func _dispatchPreconditionTest(_ condition: DispatchPredicate) -> Bool {
	switch condition {
	case .onQueue(let q):
		dispatch_assert_queue(q.__wrapped)
	case .onQueueAsBarrier(let q):
		dispatch_assert_queue_barrier(q.__wrapped)
	case .notOnQueue(let q):
		dispatch_assert_queue_not(q.__wrapped)
	}
	return true
}

@_transparent
@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
public func dispatchPrecondition(condition: @autoclosure () -> DispatchPredicate) {
	// precondition is able to determine release-vs-debug asserts where the overlay
	// cannot, so formulating this into a call that we can call with precondition()
	precondition(_dispatchPreconditionTest(condition()), "dispatchPrecondition failure")
}

/// qos_class_t

public struct DispatchQoS : Equatable {
	public let qosClass: QoSClass
	public let relativePriority: Int

	@available(OSX 10.10, iOS 8.0, *)
	public static let background = DispatchQoS(qosClass: .background, relativePriority: 0)

	@available(OSX 10.10, iOS 8.0, *)
	public static let utility = DispatchQoS(qosClass: .utility, relativePriority: 0)

	@available(OSX 10.10, iOS 8.0, *)
	public static let `default` = DispatchQoS(qosClass: .default, relativePriority: 0)

	@available(OSX, introduced: 10.10, deprecated: 10.10, renamed: "DispatchQoS.default")
	@available(iOS, introduced: 8.0, deprecated: 8.0, renamed: "DispatchQoS.default")
	@available(*, deprecated, renamed: "DispatchQoS.default")
	public static let defaultQoS = DispatchQoS.default

	@available(OSX 10.10, iOS 8.0, *)
	public static let userInitiated = DispatchQoS(qosClass: .userInitiated, relativePriority: 0)

	@available(OSX 10.10, iOS 8.0, *)
	public static let userInteractive = DispatchQoS(qosClass: .userInteractive, relativePriority: 0)

	public static let unspecified = DispatchQoS(qosClass: .unspecified, relativePriority: 0)

	public enum QoSClass {
		@available(OSX 10.10, iOS 8.0, *)
		case background

		@available(OSX 10.10, iOS 8.0, *)
		case utility

		@available(OSX 10.10, iOS 8.0, *)
		case `default`

		@available(OSX, introduced: 10.10, deprecated: 10.10, renamed: "QoSClass.default")
		@available(iOS, introduced: 8.0, deprecated: 8.0, renamed: "QoSClass.default")
		@available(*, deprecated, renamed: "QoSClass.default")
		static let defaultQoS = QoSClass.default

		@available(OSX 10.10, iOS 8.0, *)
		case userInitiated

		@available(OSX 10.10, iOS 8.0, *)
		case userInteractive

		case unspecified

		@available(OSX 10.10, iOS 8.0, *)
		internal init?(qosClass: _OSQoSClass) {
			switch qosClass {
			case .QOS_CLASS_BACKGROUND: self = .background
			case .QOS_CLASS_UTILITY: self = .utility
			case .QOS_CLASS_DEFAULT: self = .default
			case .QOS_CLASS_USER_INITIATED: self = .userInitiated
			case .QOS_CLASS_USER_INTERACTIVE: self = .userInteractive
			case .QOS_CLASS_UNSPECIFIED: self = .unspecified
			default: return nil
			}
		}

		@available(OSX 10.10, iOS 8.0, *)
		internal var rawValue: _OSQoSClass {
			switch self {
			case .background: return .QOS_CLASS_BACKGROUND
			case .utility: return .QOS_CLASS_UTILITY
			case .default: return .QOS_CLASS_DEFAULT
			case .userInitiated: return .QOS_CLASS_USER_INITIATED
			case .userInteractive: return .QOS_CLASS_USER_INTERACTIVE
			case .unspecified: return .QOS_CLASS_UNSPECIFIED
			}
		}
	}

	public init(qosClass: QoSClass, relativePriority: Int) {
		self.qosClass = qosClass
		self.relativePriority = relativePriority
	}
}

public func ==(a: DispatchQoS, b: DispatchQoS) -> Bool {
	return a.qosClass == b.qosClass && a.relativePriority == b.relativePriority
}

/// 

public enum DispatchTimeoutResult {
    static let KERN_OPERATION_TIMED_OUT:Int = 49
	case Success
	case TimedOut
}

/// dispatch_group

public extension DispatchGroup {
	public func notify(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], queue: DispatchQueue, execute work: @convention(block) () -> ()) {
		if #available(OSX 10.10, iOS 8.0, *), qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: work)
			dispatch_group_notify(self.__wrapped, queue.__wrapped, item._block)
		} else {
			dispatch_group_notify(self.__wrapped, queue.__wrapped, work)
		}
	}

	@available(OSX 10.10, iOS 8.0, *)
	public func notify(queue: DispatchQueue, work: DispatchWorkItem) {
		dispatch_group_notify(self.__wrapped, queue.__wrapped, work._block)
	}

	public func wait() {
		_ = dispatch_group_wait(self.__wrapped, DispatchTime.distantFuture.rawValue)
	}

	public func wait(timeout: DispatchTime) -> DispatchTimeoutResult {
		return dispatch_group_wait(self.__wrapped, timeout.rawValue) == 0 ? .Success : .TimedOut
	}

	public func wait(wallTimeout timeout: DispatchWallTime) -> DispatchTimeoutResult {
		return dispatch_group_wait(self.__wrapped, timeout.rawValue) == 0 ? .Success : .TimedOut
	}
}

public extension DispatchGroup {
	@available(*, deprecated, renamed: "DispatchGroup.wait(self:wallTimeout:)")
	public func wait(walltime timeout: DispatchWallTime) -> Int {
		switch wait(wallTimeout: timeout) {
		case .Success: return 0
		case .TimedOut: return DispatchTimeoutResult.KERN_OPERATION_TIMED_OUT
		}
	}
}

/// dispatch_semaphore

public extension DispatchSemaphore {
	@discardableResult
	public func signal() -> Int {
		return dispatch_semaphore_signal(self.__wrapped)
	}

	public func wait() {
		_ = dispatch_semaphore_wait(self.__wrapped, DispatchTime.distantFuture.rawValue)
	}

	public func wait(timeout: DispatchTime) -> DispatchTimeoutResult {
		return dispatch_semaphore_wait(self.__wrapped, timeout.rawValue) == 0 ? .Success : .TimedOut
	}

	public func wait(wallTimeout: DispatchWallTime) -> DispatchTimeoutResult {
		return dispatch_semaphore_wait(self.__wrapped, wallTimeout.rawValue) == 0 ? .Success : .TimedOut
	}
}

public extension DispatchSemaphore {
	@available(*, deprecated, renamed: "DispatchSemaphore.wait(self:wallTimeout:)")
	public func wait(walltime timeout: DispatchWalltime) -> Int {
		switch wait(wallTimeout: timeout) {
		case .Success: return 0
		case .TimedOut: return DispatchTimeoutResult.KERN_OPERATION_TIMED_OUT
		}
	}
}
