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
// Copyright (c) 2014-2018 Alamofire Software Foundation (http://alamofire.org/)
@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
public class UnfairLock {
    private let unfairLock: os_unfair_lock_t

    public init() {
        unfairLock = .allocate(capacity: 1)
        unfairLock.initialize(to: os_unfair_lock())
    }

    deinit {
        unfairLock.deinitialize(count: 1)
        unfairLock.deallocate()
    }

    private final func lock() {
        os_unfair_lock_lock(unfairLock)
    }

    private final func unlock() {
        os_unfair_lock_unlock(unfairLock)
    }

    /// Executes a closure returning a value while acquiring the lock.
    ///
    /// - Parameter closure: The closure to run.
    ///
    /// - Returns:           The value the closure generated.
    public final func around<T>(_ closure: () throws -> T) rethrows -> T {
        lock(); defer { unlock() }
        return try closure()
    }

    /// Execute a closure while acquiring the lock.
    ///
    /// - Parameter closure: The closure to run.
    public final func around(_ closure: () throws -> Void) rethrows {
        lock(); defer { unlock() }
        return try closure()
    }
}

/// A wrapper for data to that is mutable across threads.
@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
public class Synchronized<Wrapped>: UnfairLock {
    private var data: Wrapped

    public init(_ data: Wrapped) {
        self.data = data
    }

    /// Access to wrapped data after taking out a lock on it.
    ///
    /// - Parameter body: closure executed while wrapped data is locked
    public func synchronized<T>(_ body: (inout Wrapped) throws -> T) rethrows -> T {
        return try around { try body(&self.data) }
    }
}

/// Simple synchronized cache
@available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
public class Cached<Key: Hashable, Value>: Synchronized<[Key: Value]> {

    private let getter: (Key) -> Value

    /// Initializer
    ///
    /// - Parameter getter: a closure used when key is not already cached.
    public init(getter: @escaping (Key) -> Value) {
        self.getter = getter
        super.init([Key: Value]())
    }

    /// Get value for key, opulating if necessary
    ///
    /// - Parameter key: key of cache to fetch/generate value
    public final func get(key: Key) -> Value {
        return synchronized { data in
            if let value = data[key] {
                return value
            }

            let value = getter(key)
            data[key] = value
            return value
        }
    }
}

extension Sequence {

    /// A form of map that reads the Elements of the Sequence
    /// and processes them on the number of threads specified.
    ///
    /// - Parameter maxConcurrency: Maximum number of threads to start.
    /// - Parameter initializer: a prototype to initialize output array.
    /// - Parameter queueName: Name used for the concurrent queue.
    /// - Parameter priority: Quality of service of threads created.
    /// - Parameter worker: Closure to call to perform work.
    ///
    /// Up to maxConcurrency threads used to execute the `worker` closure
    /// passing in each value in the sequence and a closure to call when
    /// the work is complete and an output value is available.
    @available(OSX 10.12, iOS 13, tvOS 13, watchOS 6, *)
    @discardableResult
    public func concurrentMap<Output>(maxConcurrency: Int = 4,
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
