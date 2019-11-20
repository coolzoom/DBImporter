//
// Created by deamon on 02.04.18.
//

#include <cstdint>
#include <algorithm>
#include <iostream>
#include "DB2Base.h"

using namespace WDC3;

void DB2Base::process(HFileContent db2File, const std::string &fileName) {
    this->db2File = db2File;
    fileData = &(*this->db2File.get())[0];

    currentOffset = 0;
    bytesRead = 0;

    readValue(header);

    readValues(section_headers, header->section_count);
    readValues(fields, header->total_field_count);
    fieldInfoLength = header->field_storage_info_size / sizeof(field_storage_info);
    readValues(field_info, fieldInfoLength);


    palleteDataArray.resize(header->field_count);
    for (int i = 0; i < header->field_count; i++) {
        if ((field_info[i].storage_type == field_compression_bitpacked_indexed) ||
            (field_info[i].storage_type == field_compression_bitpacked_indexed_array)) {

            for (int j = 0; j < field_info[i].additional_data_size/4; j++) {
                uint32_t value;
                readValue(value);
                palleteDataArray[i].push_back(value);
            }
        }
    }


//    readValues(pallet_data, header->pallet_data_size);
    //Form hashtable for column

    commonDataHashMap.resize(header->field_count);
    for (int i = 0; i < header->field_count; i++) {
        if (field_info[i].storage_type == field_compression::field_compression_common_data)
        {
            int id;
            uint32_t value;
            for (int j = 0; j < field_info[i].additional_data_size / 8; j++) {
                readValue(id);
                readValue(value);

                commonDataHashMap[i][id] = value;
            }
        }
    }
//    readValues(common_data, header->common_data_size);

    //Read section
    for (int i = 0; i < header->section_count; i++) {
        auto &itemSectionHeader = section_headers[i];
        sections.resize(sections.size()+1);
        section &section = sections[sections.size()-1];

        if (itemSectionHeader.tact_key_hash != 0) break;

        if ((header->flags & 1) == 0) {
            // Normal records

            for (int j = 0; j < itemSectionHeader.record_count; j++) {
                record_data recordData;
                readValues(recordData.data, header->record_size);

                section.records.push_back(recordData);
            }

            readValues(section.string_data, itemSectionHeader.string_table_size);
        } else {
            // Offset map records -- these records have null-terminated strings inlined, and
            // since they are variable-length, they are pointed to by an array of 6-byte
            // offset+size pairs.
            readValues(section.variable_record_data, itemSectionHeader.offset_records_end - itemSectionHeader.file_offset);
        }

        readValues(section.id_list, itemSectionHeader.id_list_size / 4);
        if (itemSectionHeader.copy_table_count > 0) {
            readValues(section.copy_table, itemSectionHeader.copy_table_count);
        }
        readValues(section.offset_map, itemSectionHeader.offset_map_id_count);
        if (itemSectionHeader.relationship_data_size > 0) {
            // In some tables, this relationship mapping replaced columns that were used
            // only as a lookup, such as the SpellID in SpellX* tables.
            readValue(section.relationship_map.num_entries);
            readValue(section.relationship_map.min_id);
            readValue(section.relationship_map.max_id);
            readValues(section.relationship_map.entries, section.relationship_map.num_entries);
        }
        readValues(section.offset_map_id_list, itemSectionHeader.offset_map_id_count);
    }

    m_loaded = true;
}

//Returns index
int DB2Base::readRecord(int id, bool useRelationMappin, int minFieldNum, int fieldsToRead,
        std::function<void(uint32_t &recordId, int fieldNum, int subIndex, int stringOffset,unsigned char *data, size_t length)> callback) {
    //1. Get id offset
    int idDiff = id - header->min_id;

    //2. Find index
    int pos;
    auto &sectionDef = sections[0];
    if (useRelationMappin) {
        //2.1 Find index using relation mapping
        relationship_entry entry;
        entry.foreign_id = id;
        entry.record_index = 0;
        relationship_entry *start = sections[0].relationship_map.entries; //hack
        relationship_entry *end = sections[0].relationship_map.entries + (sections[0].relationship_map.num_entries - 1);
        relationship_entry *indx = std::find_if(start, end, [&id](relationship_entry &entry) -> bool {
            return entry.foreign_id == id;
        });
        pos = indx - sections[0].relationship_map.entries;
        if (indx == end || indx->foreign_id != id)
            return false;
    } else {
        //2.2 Find index in id_list

        uint32_t *end = sections[0].id_list + (section_headers->id_list_size / 4);
        uint32_t *indx = std::lower_bound(sections[0].id_list, end, id);
        pos = indx - sections[0].id_list;
        if (indx == end || *indx != id)
            return -1;
    }
    //3. Read the record
    readRecordByIndex(pos, minFieldNum, fieldsToRead, callback);

    return pos;
}

std::string DB2Base::readString(unsigned char* &fieldPointer, int sectionIndex) {
    std::string result = "";
    if ((header->flags & 1) == 0) {
        uint32_t offset = *((uint32_t *)fieldPointer) + (fieldPointer - sections[sectionIndex].string_data);
        result = std::string((char *)&sections[sectionIndex].string_data[offset]);
    } else {
        result = std::string((char *)fieldPointer);
        fieldPointer+=result.length()+1;
    }

    return result;
}

void extractBits(unsigned char *inputBuffer, unsigned char *outputBuffer, int bitOffset, int bitLength) {
    unsigned int byteOffset = (bitOffset) >> 3;
    bitOffset = bitOffset & 7;
    //Read bites

    unsigned char headMask = 0xFFu << (bitOffset);
    unsigned char tailMask = (unsigned char) (0xFFu ^ headMask);

    int totalBytesToRead = ((bitOffset+bitLength) + 7) >> 3;
    if (totalBytesToRead > 1) {
        uint8_t headByte = 0; //recordPointer[byteOffset] & (headMask);
        uint8_t tailByte = 0;

        if (bitLength > 8) {
            for (int j = 0; j < totalBytesToRead - 1; j++) {
                headByte = inputBuffer[byteOffset + j] & headMask;
                tailByte = inputBuffer[byteOffset + j + 1] & (tailMask);

                outputBuffer[j] = (headByte) >> bitOffset;
                outputBuffer[j] = outputBuffer[j] | ((tailByte) << (8 - bitOffset));
            }
        } else {
            headByte = inputBuffer[byteOffset] & headMask;
            outputBuffer[0] = (headByte) >> bitOffset;
        }

        //TODO: think through this part a little better
        if (((bitOffset + bitLength) & 7) > 0) {
            tailMask = (0xFFu >> ( 8 - ((bitOffset + bitLength) & 7)));
        } else {
            tailMask = 0xffu;
        }
        if (bitLength > 8) {
            headMask = headMask & tailMask;
            headByte = inputBuffer[byteOffset + totalBytesToRead-1] & (headMask);
            outputBuffer[totalBytesToRead-1] = outputBuffer[totalBytesToRead-1] | ((headByte) >> bitOffset);
        } else {
            tailByte = inputBuffer[byteOffset + totalBytesToRead-1] & (tailMask);
            outputBuffer[0] = outputBuffer[0] | ((tailByte) << (8 - bitOffset));
        }
//                    int endByteOffset = totalBytesToRead - 1;

    } else {
        tailMask = (0xFFu >> (8 - ((bitOffset + bitLength) & 7)));
        headMask = headMask & tailMask;

        uint8_t headByte = inputBuffer[byteOffset] & (headMask);
        outputBuffer[0] = (headByte) >> bitOffset;
    }
}

bool DB2Base::readRecordByIndex(int index, int minFieldNum, int fieldsToRead,
                                std::function<void(uint32_t &recordId, int fieldNum, int subIndex, int sectionNum, unsigned char * &data, size_t length)> callback) {
    //Find Record by section
    int sectionIndex = 0;
    while (index >= section_headers[sectionIndex].record_count) {
        index -= section_headers[sectionIndex].record_count;
        sectionIndex++;
    }


    auto &sectionDef = sections[sectionIndex];
    auto &sectionHeader = section_headers[sectionIndex];
    //
    if (sectionHeader.tact_key_hash != 0) return false;

    int numOfFieldToRead = fieldsToRead >=0 ? fieldsToRead : header->field_count;
    uint32_t recordId = 0;


    uint8_t * recordPointer = nullptr;
    if ((header->flags & 1) == 0) {
        if (sectionHeader.id_list_size > 0) {
            recordId = sectionDef.id_list[index];
        }
        recordPointer = sectionDef.records[index].data;
    } else {
        recordId = sectionDef.offset_map_id_list[index];
        recordPointer = sectionDef.variable_record_data+sectionDef.offset_map[index].offset - sectionHeader.file_offset;
//        sectionDef.variable_record_data
    }

    unsigned char *fieldDataPointer = &recordPointer[0];
    for (int i = minFieldNum; i < numOfFieldToRead; i++) {
        if ((header->flags & 1) == 0) {
            auto &fieldInfo = field_info[i];

            switch (fieldInfo.storage_type) {
                case field_compression_none: {
                    int byteOffset = fieldInfo.field_offset_bits >> 3;
                    int bytesToRead = fieldInfo.field_size_bits >> 3;

                    unsigned char *fieldDataPointer = &recordPointer[byteOffset];

                    callback(recordId, i, -1, sectionIndex, fieldDataPointer, bytesToRead);
                }
                break;


                case field_compression_bitpacked:
                case field_compression_bitpacked_signed:
                {
                    uint8_t buffer[128];

                    unsigned int bitOffset = fieldInfo.field_offset_bits;
                    unsigned int bitesToRead = fieldInfo.field_size_bits;

                    //Zero the buffer
                    for (int j = 0; j < 128; j++) buffer[j] = 0;

                    extractBits(recordPointer, &buffer[0], bitOffset, bitesToRead);

                    if (fieldInfo.storage_type == field_compression_bitpacked_signed) {
                        uint32_t signExtension = 0xFFFFFFFF << (bitesToRead);
                        uint32_t value = *((uint32_t *) &buffer[0]);
                        if (((value & (1 << (bitesToRead-1 ))) != 0)) {
                            *((uint32_t *) &buffer[0]) = (value | signExtension);
                        }
                    }


                    unsigned char *fieldDataPointer = &buffer[0];
                    callback(recordId, i, -1, sectionIndex, fieldDataPointer, bitesToRead >> 3);
                }
                break;
                case field_compression_common_data: {
                    uint32_t value = fieldInfo.field_compression_common_data.default_value;
                    //If id is found in commonData - take it from there instead of default value
                    auto it = commonDataHashMap[i].find(recordId);
                    if (it != commonDataHashMap[i].end()) {
                        value = it->second;
                    }

                    size_t bytesToRead = fieldInfo.field_size_bits >> 3;
                    uint8_t *ptr = (uint8_t *) &value;

                    callback(recordId, i, -1, sectionIndex, ptr, bytesToRead);
                }
                break;
                case field_compression_bitpacked_indexed:
                case field_compression_bitpacked_indexed_array:
                    uint8_t buffer[128];

                    unsigned int bitOffset = fieldInfo.field_compression_bitpacked_indexed.bitpacking_offset_bits;
                    unsigned int bitesToRead = fieldInfo.field_compression_bitpacked_indexed.bitpacking_size_bits;

                    //Zero the buffer
                    for (int j = 0; j < 128; j++) buffer[j] = 0;

                    extractBits(recordPointer, &buffer[0], bitOffset, bitesToRead);
                    int palleteIndex = *(uint32_t *)&buffer[0];

//                    //Todo: hackfix
//                    int properIndexForPalleteData = 0;
//                    for (int j = 0; j < i; j++) {
//                        auto const &fieldInfo = field_info[j];
//                        if ((fieldInfo.storage_type == field_compression_bitpacked_indexed) ||
//                            (fieldInfo.storage_type == field_compression_bitpacked_indexed_array)) {
//                            properIndexForPalleteData += fieldInfo.additional_data_size;
//                        }
//                    }



//                    uint8_t *ptr = reinterpret_cast<uint8_t *>(&pallet_data[properIndexForPalleteData + (palleteIndex*4)]);
                    if (fieldInfo.storage_type == field_compression_bitpacked_indexed_array) {
                        int array_count = fieldInfo.field_compression_bitpacked_indexed_array.array_count;
                        for (int j = 0; j < array_count; j++) {
                            uint32_t value = palleteDataArray[i][palleteIndex*array_count+j];
                            uint8_t *ptr = (uint8_t *) &value;

                            callback(recordId, i, j, sectionIndex, ptr, 4);
                        }
                    } else {
                        uint8_t *ptr = reinterpret_cast<uint8_t *>(&palleteDataArray[i][palleteIndex]);

                        callback(recordId, i, -1, sectionIndex, ptr, 4);
                    }

                    break;
            }

        } else {
            //variable data
            auto &fieldInfo = field_info[i];
            int bytesToRead = fieldInfo.field_size_bits >> 3;
            callback(recordId, i, -1, sectionIndex, fieldDataPointer, bytesToRead);
        }
    }


    return true;
}