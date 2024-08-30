//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import CDispatch
@_implementationOnly import _DispatchOverlayShims

public struct DispatchData : RandomAccessCollection {
	public typealias Iterator = DispatchDataIterator
	public typealias Index = Int
	public typealias Indices = DefaultIndices<DispatchData>

	public static let empty: DispatchData = DispatchData(data: _swift_dispatch_data_empty())

	public enum Deallocator {
		/// Use `free`
		case free

		/// Use `munmap`
		case unmap

		/// A custom deallocator
		// FIXME: Want @convention(block) here to minimize the overhead of
		//        doing the conversion (once per custom enum instance instead
		//        of once per call to DispatchData.init using the enum instance).
		//        However, adding the annotation here results in Data.o containing
		//        a reference to _TMBO (opaque metadata for Builtin.UnknownObject)
		//        which is only made available on platforms with Objective-C.
		case custom(DispatchQueue?, () -> Void)

		fileprivate var _deallocator: (DispatchQueue?, @convention(block) () -> Void) {
			switch self {
			case .free: return (nil, _swift_dispatch_data_destructor_free())
			case .unmap: return (nil, _swift_dispatch_data_destructor_munmap())
			case .custom(let q, let b): return (q, b)
			}
		}
	}

	internal var __wrapped: __DispatchData

	/// Initialize a `Data` with copied memory content.
	///
	/// - parameter bytes: A pointer to the memory. It will be copied.
	@available(swift, deprecated: 4, message: "Use init(bytes: UnsafeRawBufferPointer) instead")
	public init(bytes buffer: UnsafeBufferPointer<UInt8>) {
		let d = buffer.baseAddress == nil ? _swift_dispatch_data_empty()
					: dispatch_data_create(buffer.baseAddress!, buffer.count, nil,
							_swift_dispatch_data_destructor_default())
		self.init(data: d)
	}

	/// Initialize a `Data` with copied memory content.
	///
	/// - parameter bytes: A pointer to the memory. It will be copied.
	public init(bytes buffer: UnsafeRawBufferPointer) {
		let d = buffer.baseAddress == nil ? _swift_dispatch_data_empty()
					: dispatch_data_create(buffer.baseAddress!, buffer.count, nil,
							_swift_dispatch_data_destructor_default())
		self.init(data: d)
	}

	/// Initialize a `Data` without copying the bytes.
	///
	/// - parameter bytes: A buffer pointer containing the data.
	/// - parameter deallocator: Specifies the mechanism to free the indicated buffer.
	@available(swift, deprecated: 4, message: "Use init(bytesNoCopy: UnsafeRawBufferPointer, deallocater: Deallocator) instead")
	public init(bytesNoCopy bytes: UnsafeBufferPointer<UInt8>, deallocator: Deallocator = .free) {
		let (q, b) = deallocator._deallocator
		let d = bytes.baseAddress == nil ? _swift_dispatch_data_empty()
					: dispatch_data_create(bytes.baseAddress!, bytes.count, q?.__wrapped, b)
		self.init(data: d)
	}

	/// Initialize a `Data` without copying the bytes.
	///
	/// - parameter bytes: A pointer to the bytes.
	/// - parameter deallocator: Specifies the mechanism to free the indicated buffer.
	public init(bytesNoCopy bytes: UnsafeRawBufferPointer, deallocator: Deallocator = .free) {
		let (q, b) = deallocator._deallocator
		let d = bytes.baseAddress == nil ? _swift_dispatch_data_empty()
					: dispatch_data_create(bytes.baseAddress!, bytes.count, q?.__wrapped, b)
		self.init(data: d)
	}

	internal init(data: dispatch_data_t) {
		__wrapped = __DispatchData(data: data, owned: true)
	}

	internal init(borrowedData: dispatch_data_t) {
		__wrapped = __DispatchData(data: borrowedData, owned: false)
	}

	public var count: Int {
		return CDispatch.dispatch_data_get_size(__wrapped.__wrapped)
	}

	public func withUnsafeBytes<Result, ContentType>(
		body: (UnsafePointer<ContentType>) throws -> Result) rethrows -> Result
	{
		var ptr: UnsafeRawPointer? = nil
		var size = 0
		let data = CDispatch.dispatch_data_create_map(__wrapped.__wrapped, &ptr, &size)
		let contentPtr = ptr!.bindMemory(
			to: ContentType.self, capacity: size / MemoryLayout<ContentType>.stride)
		defer { _fixLifetime(data) }
		return try body(contentPtr)
	}

	@available(swift 4.2)
	public func enumerateBytes(
		_ block: (_ buffer: UnsafeBufferPointer<UInt8>, _ byteIndex: Int, _ stop: inout Bool) -> Void)
	{
		enumerateBytesCommon(block)
	}

	@available(swift, obsoleted: 4.2, renamed: "enumerateBytes(_:)")
	public func enumerateBytes(
		block: (_ buffer: UnsafeBufferPointer<UInt8>, _ byteIndex: Int, _ stop: inout Bool) -> Void)
	{
		enumerateBytesCommon(block)
	}

	private func enumerateBytesCommon(
		_ block: (_ buffer: UnsafeBufferPointer<UInt8>, _ byteIndex: Int, _ stop: inout Bool) -> Void)
	{
		// we know that capturing block in the closure being created/passed to dispatch_data_apply
		// does not cause block to escape because dispatch_data_apply does not allow its
		// block argument to escape.  Therefore, the usage of withoutActuallyEscaping to
		// bypass the Swift type system is safe.
		withoutActuallyEscaping(block) { escapableBlock in
			_ = CDispatch.dispatch_data_apply(__wrapped.__wrapped) { (_, offset: Int, ptr: UnsafeRawPointer, size: Int) in
				let bytePtr = ptr.bindMemory(to: UInt8.self, capacity: size)
				let bp = UnsafeBufferPointer(start: bytePtr, count: size)
				var stop = false
				escapableBlock(bp, offset, &stop)
				return !stop
			}
		}
	}

	/// Append bytes to the data.
	///
	/// - parameter bytes: A pointer to the bytes to copy in to the data.
	/// - parameter count: The number of bytes to copy.
	@available(swift, deprecated: 4, message: "Use append(_: UnsafeRawBufferPointer) instead")
	public mutating func append(_ bytes: UnsafePointer<UInt8>, count: Int) {
		let data = dispatch_data_create(bytes, count, nil, _swift_dispatch_data_destructor_default())
		self.append(DispatchData(data: data))
	}

	/// Append bytes to the data.
	///
	/// - parameter bytes: A pointer to the bytes to copy in to the data.
	public mutating func append(_ bytes: UnsafeRawBufferPointer) {
		// Nil base address does nothing.
		guard bytes.baseAddress != nil else { return }
		let data = dispatch_data_create(bytes.baseAddress!, bytes.count, nil, _swift_dispatch_data_destructor_default())
		self.append(DispatchData(data: data))
	}

	/// Append data to the data.
	///
	/// - parameter other: The data to append to this data.
	public mutating func append(_ other: DispatchData) {
		let data = CDispatch.dispatch_data_create_concat(__wrapped.__wrapped, other.__wrapped.__wrapped)
		__wrapped = __DispatchData(data: data, owned: true)
	}

	/// Append a buffer of bytes to the data.
	///
	/// - parameter buffer: The buffer of bytes to append. The size is calculated from `SourceType` and `buffer.count`.
	public mutating func append<SourceType>(_ buffer: UnsafeBufferPointer<SourceType>) {
		self.append(UnsafeRawBufferPointer(buffer))
	}

	private func _copyBytesHelper(to pointer: UnsafeMutableRawPointer, from range: Range<Index>) {
		var copiedCount = 0
		if range.isEmpty { return }
		let rangeSize = range.count
		_ = CDispatch.dispatch_data_apply(__wrapped.__wrapped) { (data: dispatch_data_t, offset: Int, ptr: UnsafeRawPointer, size: Int) in
			if offset >= range.endIndex { return false } // This region is after endIndex
			let copyOffset = range.startIndex > offset ? range.startIndex - offset : 0 // offset of first byte, in this region
			if copyOffset >= size { return true } // This region is before startIndex
			let count = Swift.min(rangeSize - copiedCount, size - copyOffset)
			memcpy(pointer + copiedCount, ptr + copyOffset, count)
			copiedCount += count
			return copiedCount < rangeSize
		}
	}

	/// Copy the contents of the data to a pointer.
	///
	/// - parameter pointer: A pointer to the buffer you wish to copy the bytes into.
	/// - parameter count: The number of bytes to copy.
	/// - warning: This method does not verify that the contents at pointer have enough space to hold `count` bytes.
	@available(swift, deprecated: 4, message: "Use copyBytes(to: UnsafeMutableRawBufferPointer, count: Int) instead")
	public func copyBytes(to pointer: UnsafeMutablePointer<UInt8>, count: Int) {
		_copyBytesHelper(to: pointer, from: 0..<count)
	}

	/// Copy the contents of the data to a pointer.
	///
	/// - parameter pointer: A pointer to the buffer you wish to copy the bytes into. The buffer must be large
	///	enough to hold `count` bytes.
	/// - parameter count: The number of bytes to copy.
	public func copyBytes(to pointer: UnsafeMutableRawBufferPointer, count: Int) {
		assert(count <= pointer.count, "Buffer too small to copy \(count) bytes")
		guard pointer.baseAddress != nil else { return }
		_copyBytesHelper(to: pointer.baseAddress!, from: 0..<count)
	}
		
	/// Copy a subset of the contents of the data to a pointer.
	///
	/// - parameter pointer: A pointer to the buffer you wish to copy the bytes into.
	/// - parameter range: The range in the `Data` to copy.
	/// - warning: This method does not verify that the contents at pointer have enough space to hold the required number of bytes.
	@available(swift, deprecated: 4, message: "Use copyBytes(to: UnsafeMutableRawBufferPointer, from: Range<Index>) instead")
	public func copyBytes(to pointer: UnsafeMutablePointer<UInt8>, from range: Range<Index>) {
		_copyBytesHelper(to: pointer, from: range)
	}
	
	/// Copy a subset of the contents of the data to a pointer.
	///
	/// - parameter pointer: A pointer to the buffer you wish to copy the bytes into. The buffer must be large
	///	enough to hold `count` bytes.
	/// - parameter range: The range in the `Data` to copy.
	public func copyBytes(to pointer: UnsafeMutableRawBufferPointer, from range: Range<Index>) {
		assert(range.count <= pointer.count, "Buffer too small to copy \(range.count) bytes")
		guard pointer.baseAddress != nil else { return }
		_copyBytesHelper(to: pointer.baseAddress!, from: range)
	}

	/// Copy the contents of the data into a buffer.
	///
	/// This function copies the bytes in `range` from the data into the buffer. If the count of the `range` is greater than `MemoryLayout<DestinationType>.stride * buffer.count` then the first N bytes will be copied into the buffer.
	/// - precondition: The range must be within the bounds of the data. Otherwise `fatalError` is called.
	/// - parameter buffer: A buffer to copy the data into.
	/// - parameter range: A range in the data to copy into the buffer. If the range is empty, this function will return 0 without copying anything. If the range is nil, as much data as will fit into `buffer` is copied.
	/// - returns: Number of bytes copied into the destination buffer.
	public func copyBytes<DestinationType>(to buffer: UnsafeMutableBufferPointer<DestinationType>, from range: Range<Index>? = nil) -> Int {
		let cnt = count
		guard cnt > 0 else { return 0 }
		
		let copyRange : Range<Index>
		if let r = range {
			guard !r.isEmpty else { return 0 }
			precondition(r.startIndex >= 0)
			precondition(r.startIndex < cnt, "The range is outside the bounds of the data")
			
			precondition(r.endIndex >= 0)
			precondition(r.endIndex <= cnt, "The range is outside the bounds of the data")
			
			copyRange = r.startIndex..<(r.startIndex + Swift.min(buffer.count * MemoryLayout<DestinationType>.stride, r.count))
		} else {
			copyRange = 0..<Swift.min(buffer.count * MemoryLayout<DestinationType>.stride, cnt)
		}
		
		guard !copyRange.isEmpty else { return 0 }
		
		_copyBytesHelper(to: buffer.baseAddress!, from: copyRange)
		return copyRange.count
	}

	/// Sets or returns the byte at the specified index.
	public subscript(index: Index) -> UInt8 {
		var offset = 0
		let subdata = CDispatch.dispatch_data_copy_region(__wrapped.__wrapped, index, &offset)

		var ptr: UnsafeRawPointer? = nil
		var size = 0
		let map = CDispatch.dispatch_data_create_map(subdata, &ptr, &size)
		defer { _fixLifetime(map) }

		return ptr!.load(fromByteOffset: index - offset, as: UInt8.self)
	}

	public subscript(bounds: Range<Int>) -> Slice<DispatchData> {
		return Slice(base: self, bounds: bounds)
	}

	/// Return a new copy of the data in a specified range.
	///
	/// - parameter range: The range to copy.
	public func subdata(in range: Range<Index>) -> DispatchData {
		let subrange = CDispatch.dispatch_data_create_subrange(
			__wrapped.__wrapped, range.startIndex, range.endIndex - range.startIndex)
		return DispatchData(data: subrange)
	}

	public func region(location: Int) -> (data: DispatchData, offset: Int) {
		var offset: Int = 0
		let data = CDispatch.dispatch_data_copy_region(__wrapped.__wrapped, location, &offset)
		return (DispatchData(data: data), offset)
	}

	public var startIndex: Index {
		return 0
	}

	public var endIndex: Index {
		return count
	}

	public func index(before i: Index) -> Index {
		return i - 1
	}

	public func index(after i: Index) -> Index {
		return i + 1
	}

	/// An iterator over the contents of the data.
	///
	/// The iterator will increment byte-by-byte.
	public func makeIterator() -> DispatchData.Iterator {
		return DispatchDataIterator(_data: self)
	}
}

public struct DispatchDataIterator : IteratorProtocol, Sequence {
        public typealias Element = UInt8

	/// Create an iterator over the given DispatchData
	public init(_data: DispatchData) {
		var ptr: UnsafeRawPointer?
		self._count = 0
		self._data = __DispatchData(data: CDispatch.dispatch_data_create_map(_data.__wrapped.__wrapped, &ptr, &self._count), owned: true)
		self._ptr = ptr
		self._position = _data.startIndex

		// The only time we expect a 'nil' pointer is when the data is empty.
		assert(self._ptr != nil || self._count == self._position)
	}

	/// Advance to the next element and return it, or `nil` if no next
	/// element exists.
	public mutating func next() -> DispatchData.Element? {
		if _position == _count { return nil }
		let element = _ptr.load(fromByteOffset: _position, as: UInt8.self)
		_position = _position + 1
		return element
	}

	internal let _data: __DispatchData
	internal var _ptr: UnsafeRawPointer!
	internal var _count: Int
	internal var _position: DispatchData.Index
}
