// Copyright (c) 2024 Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: MSF Parser Functions

internal MSF_Parsed *
msf_parsed_from_data(Arena *arena, String8 msf_data)
{
  Temp scratch = scratch_begin(&arena, 1);
  
  MSF_Parsed *result = 0;
  
  //- determine msf type
  U32 index_size = 0;
  if (msf_data.size >= sizeof(msf_msf20_magic) &&
      str8_match(msf_data, str8_lit(msf_msf20_magic), StringMatchFlag_RightSideSloppy)) {
    index_size = 2;
  } else if (msf_data.size >= sizeof(msf_msf70_magic) && 
             str8_match(msf_data, str8_lit(msf_msf70_magic), StringMatchFlag_RightSideSloppy)) {
    index_size = 4;
  }
  
  if (index_size == 2 || index_size == 4) {
    //- extract info from header
    U32 page_size_raw             = 0;
    U32 whole_file_page_count_raw = 0;
    U32 directory_size_raw        = 0;
    U32 directory_super_map_raw   = 0;

    if (index_size == 2) {
      MSF_Header20 *header      = (MSF_Header20 *) msf_data.str;
      page_size_raw             = header->page_size;
      whole_file_page_count_raw = header->page_count;
      directory_size_raw        = header->stream_table_size;
    } else if (index_size == 4) {
      MSF_Header70 *header      = (MSF_Header70 *) msf_data.str;
      page_size_raw             = header->page_size;
      whole_file_page_count_raw = header->page_count;
      directory_size_raw        = header->stream_table_size;
      directory_super_map_raw   = header->root_pn;
    }
    
    //- setup important sizes & counts
    
    //  (pages)
    U32 page_size = ClampTop(page_size_raw, msf_data.size);
    
    //  (whole file page count)
    U32 whole_file_page_count_max = CeilIntegerDiv(msf_data.size, page_size);
    U32 whole_file_page_count     = ClampTop(whole_file_page_count_raw, whole_file_page_count_max);
    
    //  (directory)
    U32 directory_size          = ClampTop(directory_size_raw, msf_data.size);
    U32 page_count_in_directory = CeilIntegerDiv(directory_size, page_size);
    
    //  (map)
    U32 directory_map_size           = page_count_in_directory * index_size;
    U32 page_count_in_directory_map = CeilIntegerDiv(directory_map_size, page_size);
    
    // Layout of the "directory":
    //
    // super map: [s1, s2, s3, ...]
    //       map: s1 -> [i1, i2, i3, ...]; s2 -> [...]; s3 -> [...]; ...
    // directory: i1 -> [data]; i2 -> [data]; i3 -> [data]; ... i1 -> [data]; ...
    // 
    // The "data" in the directory describes streams:
    // PDB20:
    // struct Pdb20StreamSize{
    //  U32 size;
    //  U32 unknown; // looks like kind codes or revision counters or something
    // }
    // struct{
    //  U32 stream_count;
    //  Pdb20StreamSize stream_sizes[stream_count];
    //  U16 stream_indices[stream_count][...];
    // }
    //
    // PDB70:
    // struct{
    //  U32 stream_count;
    //  U32 stream_sizes[stream_count];
    //  U32 stream_indices[stream_count][...];
    // }
    
    //- parse stream directory
    U8 *directory_buf = push_array_no_zero(scratch.arena, U8, directory_size);
    B32 got_directory = 1;
    
    {
      U32  directory_super_map_dummy     = 0;
      U32 *directory_super_map           = 0;
      U32  directory_map_page_skip_size = 0;
      if (index_size == 2) {
        directory_super_map           = &directory_super_map_dummy;
        directory_map_page_skip_size = OffsetOf(MSF_Header20, stream_table_size);
      } else {
        U64 super_map_off   = OffsetOf(MSF_Header70, root_pn);
        directory_super_map = (U32 *) (msf_data.str + super_map_off);
      }
      
      U32 max_index_count_in_map_page = (page_size - directory_map_page_skip_size) / index_size;
      
      // for each index in super map ...
      U8  *out_ptr       = directory_buf;
      U32 *super_map_ptr = directory_super_map;
      for (U32 i = 0; i < page_count_in_directory_map; ++i, ++super_map_ptr) {
        U32 directory_map_page_index = *super_map_ptr;
        if (directory_map_page_index >= whole_file_page_count) {
          got_directory = 0;
          goto parse_directory_done;
        }
        
        U64 directory_map_page_off  = ((U64) directory_map_page_index) * page_size;
        U8 *directory_map_page_base = msf_data.str + directory_map_page_off;
        
        // clamp index count by end of directory
        U32 index_count;
        {
          U32 directory_pos             = (U32)(out_ptr - directory_buf);
          U32 remaining_size            = directory_size - directory_pos;
          U32 remaining_map_page_count = CeilIntegerDiv(remaining_size, page_size);
          index_count = ClampTop(max_index_count_in_map_page, remaining_map_page_count);
        }
        
        // for each index in map ...
        U8 *map_ptr = directory_map_page_base + directory_map_page_skip_size;
        for (U32 j = 0; j < index_count; ++j, map_ptr += index_size) {
          
          // read index
          U32 directory_page_index = 0;
          if (index_size == 4) {
            directory_page_index = *(U32 *) map_ptr;
          } else {
            directory_page_index = *(U16 *) map_ptr;
          }
          if (directory_page_index >= whole_file_page_count) {
            got_directory = 0;
            goto parse_directory_done;
          }
          
          U64 directory_page_off  = ((U64) directory_page_index) * page_size;
          U8 *directory_page_base = msf_data.str + directory_page_off;
          
          // clamp copy size by end of directory
          U32 copy_size;
          {
            U32 directory_pos  = (U32) (out_ptr - directory_buf);
            U32 remaining_size = directory_size - directory_pos;
            copy_size          = ClampTop(page_size, remaining_size);
          }
          
          // copy page data
          MemoryCopy(out_ptr, directory_page_base, copy_size);
          out_ptr += copy_size;
        }
        
      }
      
      parse_directory_done:;
    }
    
    //- parse streams from directory
    U32      stream_count = 0;
    B32      got_streams  = 0;
    String8 *streams      = 0;
    
    if (got_directory) {
      got_streams = 1;
      
      // read stream count
      U32 stream_count_raw = *(U32 *) directory_buf;
      
      // setup counts, sizes, and offsets
      U32 size_of_stream_entry   = index_size == 2 ? 8 : 4;
      U32 stream_count_max       = (directory_size - 4) / size_of_stream_entry;
      U32 stream_count__inner    = ClampTop(stream_count_raw, stream_count_max);
      U32 all_stream_entries_off = 4;
      U32 all_indices_off        = all_stream_entries_off + (stream_count__inner * size_of_stream_entry);
      
      // set output buffer and count
      stream_count = stream_count__inner;
      streams      = push_array_no_zero(arena, String8, stream_count);
      
      // iterate sizes and indices in lock step
      U32      entry_cursor = all_stream_entries_off;
      U32      index_cursor = all_indices_off;
      String8 *stream_ptr   = streams;
      for (U32 i = 0; i < stream_count; ++i) {
        // read stream size
        U32 stream_size_raw = *(U32 *) (directory_buf + entry_cursor);
        if (stream_size_raw == MSF_DELETED_STREAM_STAMP) {
          stream_size_raw = 0;
        }
        
        // compute page count
        U32 stream_page_count_raw = CeilIntegerDiv(stream_size_raw, page_size);
        U32 stream_page_count_max = (directory_size - index_cursor) / index_size;;
        U32 stream_page_count     = ClampTop(stream_page_count_raw, stream_page_count_max);
        U32 stream_size            = ClampTop(stream_size_raw, stream_page_count*page_size);
        
        // copy stream data
        U8 *stream_buf = push_array_no_zero(arena, U8, stream_size);
        stream_ptr->str  = stream_buf;
        stream_ptr->size = stream_size;
        
        U32  sub_index_cursor = index_cursor;
        U8  *stream_out_ptr   = stream_buf;
        for (U32 i = 0; i < stream_page_count; ++i, sub_index_cursor += index_size) {
          
          // read index
          U32 stream_page_index = 0;
          if (index_size == 4) {
            stream_page_index = *(U32 *) (directory_buf + sub_index_cursor);
          } else {
            stream_page_index = *(U16 *) (directory_buf + sub_index_cursor);
          }
          if (stream_page_index >= whole_file_page_count) {
            got_streams = 0;
            goto parse_streams_done;
          }
          
          U64  stream_page_off  = ((U64) stream_page_index) * page_size;
          U8  *stream_page_base = msf_data.str + stream_page_off;
          
          // clamp copy size by end of stream
          U32 copy_size;
          {
            U32 stream_pos     = (U32) (stream_out_ptr - stream_buf);
            U32 remaining_size = stream_size - stream_pos;
            copy_size          = ClampTop(page_size, remaining_size);
          }
          
          // copy page data
          MemoryCopy(stream_out_ptr, stream_page_base, copy_size);
          stream_out_ptr += copy_size;
        }
        
        // advance cursors
        entry_cursor += size_of_stream_entry;
        index_cursor  = sub_index_cursor;
        stream_ptr   += 1;
      }
      
      parse_streams_done:;
    }
    
    if (got_streams) {
      result               = push_array_no_zero(arena, MSF_Parsed, 1);
      result->streams      = streams;
      result->stream_count = stream_count;
      result->block_size   = page_size;
      result->block_count  = whole_file_page_count;
    }
  }
  
  scratch_end(scratch);
  return result;
}

internal String8
msf_data_from_stream(MSF_Parsed *msf, MSF_StreamNumber sn)
{
  String8 result = {0};
  if(sn < msf->stream_count)
  {
    result = msf->streams[sn];
  }
  return(result);
}