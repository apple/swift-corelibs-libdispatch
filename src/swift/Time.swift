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

// dispatch/time.h
// DISPATCH_TIME_NOW: ok
// DISPATCH_TIME_FOREVER: ok

import CDispatch

public struct DispatchTime : Comparable {
#if HAVE_MACH
	private static let timebaseInfo: mach_timebase_info_data_t = {
		var info = mach_timebase_info_data_t(numer: 1, denom: 1)
		mach_timebase_info(&info)
		return info
	}()
#endif
 
	public let rawValue: dispatch_time_t

	public static func now() -> DispatchTime {
		let t = CDispatch.dispatch_time(0, 0)
		return DispatchTime(rawValue: t)
	}

	public static let distantFuture = DispatchTime(rawValue: ~0)

	fileprivate init(rawValue: dispatch_time_t) {
		self.rawValue = rawValue
	}

	/// Creates a `DispatchTime` relative to the system clock that
	/// ticks since boot.
	///
	/// - Parameters:
	///   - uptimeNanoseconds: The number of nanoseconds since boot, excluding
	///                        time the system spent asleep
	/// - Returns: A new `DispatchTime`
	/// - Discussion: This clock is the same as the value returned by
	///               `mach_absolute_time` when converted into nanoseconds.
	///               On some platforms, the nanosecond value is rounded up to a
	///               multiple of the Mach timebase, using the conversion factors
	///               returned by `mach_timebase_info()`. The nanosecond equivalent
	///               of the rounded result can be obtained by reading the
	///               `uptimeNanoseconds` property.
	///               Note that `DispatchTime(uptimeNanoseconds: 0)` is
	///               equivalent to `DispatchTime.now()`, that is, its value
	///               represents the number of nanoseconds since boot (excluding
	///               system sleep time), not zero nanoseconds since boot.
	public init(uptimeNanoseconds: UInt64) {
		var rawValue = uptimeNanoseconds
#if HAVE_MACH
		if (DispatchTime.timebaseInfo.numer != DispatchTime.timebaseInfo.denom) {
			rawValue = (rawValue * UInt64(DispatchTime.timebaseInfo.denom) 
				+ UInt64(DispatchTime.timebaseInfo.numer - 1)) / UInt64(DispatchTime.timebaseInfo.numer)
		}
#endif
		self.rawValue = dispatch_time_t(rawValue)
	}

	public var uptimeNanoseconds: UInt64 {
		var result = self.rawValue
#if HAVE_MACH
		if (DispatchTime.timebaseInfo.numer != DispatchTime.timebaseInfo.denom) {
			result = result * UInt64(DispatchTime.timebaseInfo.numer) / UInt64(DispatchTime.timebaseInfo.denom)
		}
#endif
		return result
	}
}

public func <(a: DispatchTime, b: DispatchTime) -> Bool {
	if a.rawValue == ~0 || b.rawValue == ~0 { return false }
	return a.rawValue < b.rawValue
}

public func ==(a: DispatchTime, b: DispatchTime) -> Bool {
	return a.rawValue == b.rawValue
}

public struct DispatchWallTime : Comparable {
	public let rawValue: dispatch_time_t

	public static func now() -> DispatchWallTime {
		return DispatchWallTime(rawValue: CDispatch.dispatch_walltime(nil, 0))
	}

	public static let distantFuture = DispatchWallTime(rawValue: ~0)

	fileprivate init(rawValue: dispatch_time_t) {
		self.rawValue = rawValue
	}

	public init(timespec: timespec) {
		var t = timespec
		self.rawValue = CDispatch.dispatch_walltime(&t, 0)
	}
}

public func <(a: DispatchWallTime, b: DispatchWallTime) -> Bool {
	if b.rawValue == ~0 {
		return a.rawValue != ~0
	} else if a.rawValue == ~0 {
		return false
	}
	return -Int64(bitPattern: a.rawValue) < -Int64(bitPattern: b.rawValue)
}

public func ==(a: DispatchWallTime, b: DispatchWallTime) -> Bool {
	return a.rawValue == b.rawValue
}

public enum DispatchTimeInterval {
	case seconds(Int)
	case milliseconds(Int)
	case microseconds(Int)
	case nanoseconds(Int)
	@_downgrade_exhaustivity_check
	case never

	internal var rawValue: Int64 {
		switch self {
		case .seconds(let s): return Int64(s) * Int64(NSEC_PER_SEC)
		case .milliseconds(let ms): return Int64(ms) * Int64(NSEC_PER_MSEC)
		case .microseconds(let us): return Int64(us) * Int64(NSEC_PER_USEC)
		case .nanoseconds(let ns): return Int64(ns)
		case .never: return Int64.max
		}
	}

	public static func ==(lhs: DispatchTimeInterval, rhs: DispatchTimeInterval) -> Bool {
		switch (lhs, rhs) {
		case (.never, .never): return true
		case (.never, _): return false
		case (_, .never): return false
		default: return lhs.rawValue == rhs.rawValue
		}
	}
}

public func +(time: DispatchTime, interval: DispatchTimeInterval) -> DispatchTime {
	let t = CDispatch.dispatch_time(time.rawValue, interval.rawValue)
	return DispatchTime(rawValue: t)
}

public func -(time: DispatchTime, interval: DispatchTimeInterval) -> DispatchTime {
	let t = CDispatch.dispatch_time(time.rawValue, -interval.rawValue)
	return DispatchTime(rawValue: t)
}

public func +(time: DispatchTime, seconds: Double) -> DispatchTime {
	let interval = seconds * Double(NSEC_PER_SEC)
	let t = CDispatch.dispatch_time(time.rawValue,
		interval.isInfinite || interval.isNaN ? Int64.max : Int64(interval))
	return DispatchTime(rawValue: t)
}

public func -(time: DispatchTime, seconds: Double) -> DispatchTime {
	let interval = -seconds * Double(NSEC_PER_SEC)
	let t = CDispatch.dispatch_time(time.rawValue,
		interval.isInfinite || interval.isNaN ? Int64.min : Int64(interval))
	return DispatchTime(rawValue: t)
}

public func +(time: DispatchWallTime, interval: DispatchTimeInterval) -> DispatchWallTime {
	let t = CDispatch.dispatch_time(time.rawValue, interval.rawValue)
	return DispatchWallTime(rawValue: t)
}

public func -(time: DispatchWallTime, interval: DispatchTimeInterval) -> DispatchWallTime {
	let t = CDispatch.dispatch_time(time.rawValue, -interval.rawValue)
	return DispatchWallTime(rawValue: t)
}

public func +(time: DispatchWallTime, seconds: Double) -> DispatchWallTime {
	let interval = seconds * Double(NSEC_PER_SEC)
	let t = CDispatch.dispatch_time(time.rawValue,
		interval.isInfinite || interval.isNaN ? Int64.max : Int64(interval))
	return DispatchWallTime(rawValue: t)
}

public func -(time: DispatchWallTime, seconds: Double) -> DispatchWallTime {
	let interval = -seconds * Double(NSEC_PER_SEC)
	let t = CDispatch.dispatch_time(time.rawValue,
		interval.isInfinite || interval.isNaN ? Int64.min : Int64(interval))
	return DispatchWallTime(rawValue: t)
}
