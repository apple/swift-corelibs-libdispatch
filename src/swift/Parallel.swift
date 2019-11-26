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

import Dispatch

/// Wrapper for os_unfair_lock mutex primitve from the
/// project: https://github.com/Alamofire/Alamofire
@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
public final class UnfairLock {
    private let unfairLock: os_unfair_lock_t

    public init() {
        unfairLock = .allocate(capacity: 1)
        unfairLock.initialize(to: os_unfair_lock())
    }

    deinit {
        unfairLock.deinitialize(count: 1)
        unfairLock.deallocate()
    }

    private func lock() {
        os_unfair_lock_lock(unfairLock)
    }

    private func unlock() {
        os_unfair_lock_unlock(unfairLock)
    }

    /// Executes a closure returning a value while acquiring the lock.
    ///
    /// - Parameter closure: The closure to run.
    ///
    /// - Returns:           The value the closure generated.
    func synchronized<T>(_ closure: () -> T) -> T {
        lock(); defer { unlock() }
        return closure()
    }

    /// Execute a closure while acquiring the lock.
    ///
    /// - Parameter closure: The closure to run.
    func synchronized(_ closure: () -> Void) {
        lock(); defer { unlock() }
        return closure()
    }
}

public typealias SynchronizableLock = UnfairLock
private var allLocks = [OpaquePointer: SynchronizableLock]()
private var lockLock = SynchronizableLock()

protocol Synchronizable {
    mutating func synchronized<T>(_ closure: (inout Self) -> T) -> T
}

extension Dictionary: Synchronizable {}
extension Array: Synchronizable {}
extension Int: Synchronizable {}

extension Synchronizable {

    public mutating func synchronized<T>(_ closure: (inout Self) -> T) -> T {
        let lockee = UnsafeMutablePointer(&self)
        let lock = lockLock.synchronized { () -> SynchronizableLock in
            let key = OpaquePointer(lockee)
            var lock = allLocks[key]
            if lock == nil {
                lock = SynchronizableLock()
                allLocks[key] = lock
            }
            return lock!
        }
        return lock.synchronized { closure(&lockee.pointee) }
    }

    public mutating func desynchronize() {
        let lockee = UnsafeMutablePointer(&self)
        lockLock.synchronized {
            allLocks[OpaquePointer(lockee)] = nil
        }
    }
}

/// Property wrapper to make read/write access to the
/// wrapped value synchronous across multiple threads.
@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
@propertyWrapper
public struct Atomic<Value> {

    private let lock = UnfairLock()
    private var _stored: Value

    public init(wrappedValue initialValue: Value) {
      _stored = initialValue
    }

    public var wrappedValue: Value {
        get {
            return lock.synchronized { _stored }
        }
        set(newValue) {
            lock.synchronized {
                _stored = newValue
            }
        }
    }
}

extension Sequence {

    /// A form of map that reads the Elements of the Sequence
    /// and processes them on the number of threads specified.
    ///
    /// - Parameter maxConcurrency: Maximum number of threads to start.
    /// - Parameter queueName: Name used for the concurrent queue
    /// - Parameter priority: Quality of service of threads created
    /// - Parameter worker: Closure to call to perform work
    ///
    /// Up to maxConcurrency threads used to execute the `worker` closure
    /// passing in each value in the sequence and a closure to call when
    /// the work is complete and an output value is available.
    @available(OSX 10.12, iOS 13, tvOS 13, watchOS 6, *)
    @discardableResult
    func concurrentMap<Output>(maxConcurrency: Int = 4,
                               initializer: Output? = nil,
                               queueName: String = "concurrentMap",
                               priority: DispatchQoS = .default,
                               worker: @escaping (Element,
                            @escaping (Output) -> Void) -> Void) -> [Output] {
        let input = Array(self)
        var output: [Output]
        if initializer != nil {
            output = Array(repeating: initializer!, count: input.count)
        } else {
            output = Array(unsafeUninitializedCapacity: input.count,
                          initializingWith: {
                            (buffer, initializedCount) in
                            let stride = MemoryLayout<Output>.stride
                            memset(buffer.baseAddress, 0,
                                   stride * input.count)
                            initializedCount = input.count})
        }

        output.withUnsafeMutableBufferPointer {
            buffer in
            let buffro = buffer
            let concurrentQueue = DispatchQueue(label: queueName, qos: priority,
                                                attributes: .concurrent)
            // Semaphore regulates the maximum number of active threads
            let semaphore = DispatchSemaphore(value: maxConcurrency)
            // ThreadGroup waits for running threads to complete
            let threadGroup = DispatchGroup()

            for index in 0..<input.count {
                threadGroup.enter()
                semaphore.wait()

                concurrentQueue.async {
                    worker(input[index], { result in
                        buffro[index] = result
                        threadGroup.leave()
                        semaphore.signal()
                    })
                }
            }

            threadGroup.wait()
        }

        return output
    }
}
