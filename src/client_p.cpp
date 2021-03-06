#include <cmath> /// For log2 function
#include <cstdlib> /// memcpy
#include <algorithm>
#include <boost/algorithm/string.hpp>

#include <iostream>

#include "../include/client_p.hpp"

using namespace Zsync3;

//// Helper functions
static std::string &ltrim(std::string &str) {
    auto it2 =  std::find_if(
                    str.begin(),
    str.end(), [](char ch) {
        return !std::isspace<char>(ch, std::locale::classic() ) ;
    });
    str.erase(str.begin(), it2);
    return str;
}

static std::string &rtrim(std::string &str) {
    auto it1 =  std::find_if(str.rbegin(), str.rend(),
    [](char ch) {
        return !std::isspace<char>(ch, std::locale::classic() );
    });
    str.erase(it1.base(), str.end());
    return str;
}
//// ---

ClientPrivate::ClientPrivate() {
    md4_ctx.reset(new MD4_CTX);
    sha1_ctx.reset(new SHA_CTX);

    /// Initializes hashing contexts.
    MD4_Init(md4_ctx.get());
    SHA1_Init(sha1_ctx.get());
}
ClientPrivate::~ClientPrivate() { }

bool ClientPrivate::SetMetaFile(const std::string &path) {
    std::unique_ptr<std::ifstream> meta_file(new std::ifstream);
    meta_file->open(path, std::ios::in | std::ios::binary);
    if(!meta_file->is_open()) {
        return false;
    }

    const std::string header_delimiter = ":";
    const std::string hash_lengths_delimiter = ",";
    const int buf_size = 200;

    size_t pos = 0;
    std::vector<char> buffer;
    buffer.resize(buf_size);

    while(!meta_file->eof()) {
        /// Clear garbage data.
        std::fill(buffer.begin(), buffer.end(), '\0');

        meta_file->getline(buffer.data(), buf_size);
        if(meta_file->fail()) {
		return false;
	}

	std::string line(buffer.data());

        if(line.length() == 0) {
            ///// 0x0a 0x0a represents two new lines which proves the start of
            ///// Checksum blocks in the zsync meta file.

            ///// IMPORTANT:
            ///// Set Target Total Blocks Count
            num_blocks = (target_file_length + blocksize - 1) / blocksize;

            //// Let's read the checksums from the zsync meta file and construct
            //// our required hash tables.

            //// We need to allocate the block hashes first.
            try {
                block_hashes.reset(new std::vector<HashEntry>);
                block_hashes->resize(num_blocks + num_seq_matches);
            } catch(...) {
                return false;
            }

            std::vector<char> md4_buffer;
            md4_buffer.resize(16); // MD4 transmitted is atmost 16 bytes long.

            std::vector<uint16_t> rsum_buffer;
            rsum_buffer.resize(2); // Rsum transmitted is atmost 4 bytes long.

            for(int64_t id = 0; id < num_blocks && !meta_file->eof(); ++id) {
                /// Clear all previous values
                std::fill(md4_buffer.begin(), md4_buffer.end(), '\0');
                std::fill(rsum_buffer.begin(), rsum_buffer.end(), 0);

                // Read on.

                //// Read Adler32 Checksum. Only transmitted ones.
                meta_file->read(
                    ((char *)rsum_buffer.data()) + 4 - num_weak_checksum_bytes,
                    num_weak_checksum_bytes
                );

                //// Read Strong Checksum(MD4 for now)
                meta_file->read(md4_buffer.data(), num_strong_checksum_bytes);

                if(meta_file->fail()) {
                    return false;
                }

                //// Get hash entry and fill in required data.
                HashEntry *e = &(block_hashes->at(id));
                e->block_id = id;
		e->rsum = RollingChecksum(rsum_buffer[0], rsum_buffer[1]);
		//// For hashing we need the b of the next rsum so we store
		//// it.
		if(num_seq_matches > 1 && id > 0) {
			block_hashes->at(id - 1).rsum.nb = e->rsum.b;
		}
                e->md4 = md4_buffer;
            }
            break;
        }

        pos = line.find(header_delimiter);
        if(pos == std::string::npos) {
            return false;
        }

        std::string header = line.substr(0, pos);
        std::string header_value = line.erase(0,
                                              pos + header_delimiter.length());
        header_value = ltrim(rtrim(header_value));

        ///// Now parse zsync headers
        boost::algorithm::to_lower(header); /// Convert the header to lower case.

        if(header == "zsync") { //// Zsync Version String
            zs_version = header_value;
        } else if(header == "filename") { //// Target file name.
            target_filename = header_value;
        } else if(header == "mtime") { //// MTime of zsync meta file creation.
            mtime = header_value; //// For now MTime is not needed that much.
        } else if(header == "blocksize") { /// Get blocksize
            blocksize = std::stoi(header_value);  /// Convert to integer.
        } else if(header == "length") { /// Set file length
            target_file_length = std::stoi(header_value);
        } else if(header == "hash-lengths") { //// Set weak checksum bytes and strong checksum bytes
            std::string len_value;
            std::string value = header_value;
            std::vector<int> hash_lengths;
            pos = 0;
            while((pos = value.find(hash_lengths_delimiter)) != std::string::npos) {
                len_value = value.substr(0, pos);
                hash_lengths.push_back(std::stoi(len_value));
                value = value.erase(0, pos + hash_lengths_delimiter.length());
            }
            hash_lengths.push_back(std::stoi(len_value));

            if(hash_lengths.size() != 3) {
                return false;
            }

            num_seq_matches = hash_lengths[0];
            num_weak_checksum_bytes = hash_lengths[1];
            num_strong_checksum_bytes = hash_lengths[2];
        } else if(header == "url") {
            target_file_url = header_value;
        } else if(header == "sha-1") {
            target_file_sha1 = header_value;
        } else {
            return false;
        }
    }
    ///// Build the Rsum Mapping.
    BuildRsumHashTable();

    ///// Calculate and Set Precalculated Values.
    context = blocksize * num_seq_matches;
    blockshift = (blocksize == 1024) ? 10 :
                 (blocksize == 2048) ? 11 :
                 log2(blocksize);
       
    //// Construct temporary files.

    return true;
}

//// Only be called after Set Meta File.
bool ClientPrivate::SubmitSeedFile(const std::string &path) {
    std::unique_ptr<std::ifstream> seed_file(new std::ifstream);
    seed_file->open(path, std::ios::in | std::ios::binary);
    if(!seed_file->is_open()) {
        return false;
    }

    const int bufsize = blocksize * 16;
    int in = 0;

    std::vector<char> buffer;
    buffer.resize(bufsize + context);
    std::fill(buffer.begin(), buffer.end(), '\0');

    while(!seed_file->eof()) {
        size_t len = 0;
        int64_t start_in = in;

        //// If this is the first read then fill the buffer
        if(!in) {
            seed_file->read(buffer.data(), bufsize);
            len = seed_file->gcount();
            in += len;
        }
        //// Else copy the last context number of bytes to the begining
        //// and then fill the rest of buffer with stream data.
        else {
            memcpy(buffer.data(), buffer.data() + (bufsize - context), context);
            in += bufsize - context;
            seed_file->read(buffer.data() + context, bufsize - context);
            len = context + seed_file->gcount();
        }

        //// Pad with zeros if we read everything from the stream.
        if(seed_file->eof()) {
            memset(buffer.data() + len, 0, context);
            len += context;
        }

        ///// Submit data to be checked and written.
        SubmitSourceData(buffer.data(), len, start_in);
    }
    return true;
}

///// =========================
///// Private Methods

int64_t ClientPrivate::SubmitSourceData(const char *buffer, size_t len, int64_t start) {
    //// TODO: implement this with all the variables in this method scope.
    (void)buffer;
    (void)len;
    (void)start; 
    return 0;
}

bool ClientPrivate::BuildRsumHashTable() {
    int i = 16; /// Mapping should be inside the possible values of Addler-32's first 16 bits.
    //// We step down i till we have sufficient number to hold all blocks
    //// Order of the hash table is 2^i such that i is minimum which can all blocks
    //// and not less than 4.
    while ((2 << (i - 1)) > num_blocks && i > 4) {
        i--;
    }

    hash_mask = (2 << i) - 1;

    //// Initialize the hasher.
    auto hasher = RollingChecksumHasher(weak_checksum_mask, hash_mask, num_seq_matches);

    //// Allocate the rsum hash tables based on rsum.
    try {
        rsum_map.reset(
		new std::unordered_multimap<RollingChecksum, HashEntry*, RollingChecksumHasher, RollingChecksumEqual> (
			(int)(hash_mask + 1),
			hasher
		)	
	);
    } catch(...) {
        return false;
    }

    //// Now fill in the map.
    for (int64_t id = num_blocks; id > 0;) {
        HashEntry *e = &(block_hashes->at(--id));	
	rsum_map->insert({ e->rsum, e});
    }
    return true;
}
