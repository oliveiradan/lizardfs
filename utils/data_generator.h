#pragma once

#include <endian.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "utils/asserts.h"
#include "utils/configuration.h"

/*
 * This class generates files in the following format:
 * - first 8 bytes: size of the file (thus the minimal size is 8 bytes)
 * - then a sequence of 8-byte blocks, each block contains a value
 *     (offset + 0x0807060504030201) % 2^64
 * If the file size does not divide by 8 the last block is truncated.
 * All numbers (uint64) are in big endian format
 * (it is easier for a human to read the hexdump -C of such file)
 */
class DataGenerator {
public:
	static void createFile(const std::string& name, uint64_t size) {
		int fd = open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, (mode_t)0644);
		utils_passert(fd >= 0);
		fillFileWithProperData(fd, size);
		utils_zassert(close(fd));
	}

	static void overwriteFile(const std::string& name) {
		struct stat fileInformation;
		utils_zassert(stat(name.c_str(), &fileInformation));
		int fd = open(name.c_str(), O_WRONLY, (mode_t)0644);
		utils_passert(fd >= 0);
		fillFileWithProperData(fd, fileInformation.st_size);
		utils_zassert(close(fd));
	}

	/*
	 * This function checks if the file contains proper data
	 * generated by DataStream::createFile and throws an std::exception
	 * in case of the data is corrupted
	 */
	static void validateFile(const std::string& name) {
		int fd = open(name.c_str(), O_RDONLY);
		utils_passert(fd != -1);
		std::string error;

		/* Check the file size */
		struct stat fileInformation;
		utils_zassert(stat(name.c_str(), &fileInformation));
		uint64_t actualSize = fileInformation.st_size;
		uint64_t expectedSize;
		if(read(fd, &expectedSize, sizeof(expectedSize)) != sizeof(expectedSize)) {
			// The file if too short, so the first bytes are corrupted.
			throw std::length_error("file too short (" + std::to_string(actualSize) + " bytes)");
		}
		expectedSize = be64toh(expectedSize);
		if (expectedSize != (uint64_t)fileInformation.st_size) {
			error = "file should be " + std::to_string(expectedSize) +
					" bytes long, but is " + std::to_string(actualSize) + " bytes long\n";
		}

		/* Check the data */
		off_t currentOffset = sizeof(actualSize);
		uint64_t size = actualSize - sizeof(actualSize);
		std::vector<char> actualBuffer(UtilsConfiguration::blockSize());
		std::vector<char> properBuffer(UtilsConfiguration::blockSize());
		while (size > 0) {
			uint64_t bytesToRead = size;
			if (bytesToRead > properBuffer.size()) {
				bytesToRead = properBuffer.size();
			}
			properBuffer.resize(bytesToRead);
			fillBufferWithProperData(properBuffer, currentOffset);
			utils_passert(read(fd, actualBuffer.data(), bytesToRead) == (ssize_t)bytesToRead);
			size -= bytesToRead;
			// memcmp is very fast, use it to check if everything is OK
			if (memcmp(actualBuffer.data(), properBuffer.data(), bytesToRead) == 0) {
				currentOffset += bytesToRead;
				continue;
			}
			// if not -- find the byte which is corrupted
			for (size_t i = 0; i < bytesToRead; ++i) {
				if (actualBuffer[i] != properBuffer[i]) {
					std::stringstream ss;
					ss << "data mismatch at offset " << i << ". Expected/actual:\n";
					for (size_t j = i; j < bytesToRead && j < i + 32; ++j) {
						ss << std::hex << std::setfill('0') << std::setw(2)
								<< static_cast<int>(static_cast<unsigned char>(properBuffer[j]))
								<< " ";
					}
					ss << "\n";
					for (size_t j = i; j < bytesToRead && j < i + 32; ++j) {
						ss << std::hex << std::setfill('0') << std::setw(2)
								<< static_cast<int>(static_cast<unsigned char>(actualBuffer[j]))
								<< " ";
					}
					throw std::invalid_argument(error + ss.str());
				}
			}
			utils_mabort("memcmp returned non-zero, but there is no difference");
		}
		utils_zassert(close(fd));
		if (!error.empty()) {
			throw std::length_error(error + "The rest of the file is OK");
		}
	}


protected:
	static void fillBufferWithProperData(std::vector<char>& buffer, off_t offset) {
		size_t size = buffer.size();
		if (offset % 8 == 0 && size % 8 == 0) {
			fillAlignedBufferWithProperData(buffer, offset);
			return;
		}
		/*
		 * If the buffer or offset is not aligned
		 * we will create aligned buffer big enough to
		 * be a superset of the buf, fill it with data
		 * and copy the proper part of it
		 */
		off_t alignedOffset = (offset / 8) * 8;
		std::vector<char> alignedBuffer(16 + (size / 8) * 8);
		fillAlignedBufferWithProperData(alignedBuffer, alignedOffset);
		memcpy(buffer.data(), alignedBuffer.data() + (offset - alignedOffset), size);
	}

	/**
	 * This function requires both offset and size to be multiples of 8
	 */
	static void fillAlignedBufferWithProperData(std::vector<char>& buffer, off_t offset) {
		typedef uint64_t BlockType;
		utils_massert(sizeof(BlockType) == 8);
		utils_massert(offset % sizeof(BlockType) == 0);
		utils_massert(buffer.size() % sizeof(BlockType) == 0);
		BlockType *blocks = (BlockType*)buffer.data();
		for (size_t i = 0; i < buffer.size() / sizeof(BlockType); ++i) {
			BlockType block = htobe64(0x0807060504030201ULL + offset);
			blocks[i] = block;
			offset += sizeof(BlockType);
		}
	}

	static void fillFileWithProperData(int fd, uint64_t size) {
		utils_massert(fd >= 0);

		/* Write the size of the file */
		uint64_t serializedSize = htobe64(size);
		utils_massert(size >= sizeof(serializedSize));
		utils_passert(write(fd, &serializedSize, sizeof(serializedSize))== sizeof(serializedSize));

		/* Write the data */
		size -= sizeof(serializedSize);
		off_t currentOffset = sizeof(serializedSize);
		std::vector<char> buffer(UtilsConfiguration::blockSize());
		while (size > 0) {
			size_t bytesToWrite = size;
			if (bytesToWrite > buffer.size()) {
				bytesToWrite = buffer.size();
			}
			buffer.resize(bytesToWrite);
			fillBufferWithProperData(buffer, currentOffset);
			utils_passert(write(fd, buffer.data(), bytesToWrite) == (ssize_t)bytesToWrite);
			size -= bytesToWrite;
			currentOffset += bytesToWrite;
		}
	}
};
