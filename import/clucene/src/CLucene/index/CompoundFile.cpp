/*------------------------------------------------------------------------------
* Copyright (C) 2003-2006 Ben van Klinken and the CLucene Team
* 
* Distributable under the terms of either the Apache License (Version 2.0) or 
* the GNU Lesser General Public License, as specified in the COPYING file.
------------------------------------------------------------------------------*/
#include "CLucene/StdHeader.h"
#include "CompoundFile.h"
#include "CLucene/util/Misc.h"

CL_NS_USE(store)
CL_NS_USE(util)
CL_NS_DEF(index)

CompoundFileReader::CSInputStream::CSInputStream(CL_NS(store)::IndexInput* base, const int64_t fileOffset, const int64_t length){
   this->base = base;
   this->fileOffset = fileOffset;
   this->_length = length;   // variable in the superclass
}
	
void CompoundFileReader::CSInputStream::readInternal(uint8_t* b, const int32_t offset, const int32_t len)
{
   SCOPED_LOCK_MUTEX(base->THIS_LOCK)

   int64_t start = getFilePointer();
   if(start + len > _length)
      _CLTHROWA(CL_ERR_IO,"read past EOF");
   base->seek(fileOffset + start);
   base->readBytes(b, offset, len);
}
CompoundFileReader::CSInputStream::~CSInputStream(){
}
IndexInput* CompoundFileReader::CSInputStream::clone() const
{
	return _CLNEW CSInputStream(*this);
}
CompoundFileReader::CSInputStream::CSInputStream(const CSInputStream& clone): BufferedIndexInput(clone){
   this->base = clone.base; //no need to clone this..
   this->fileOffset = clone.fileOffset;
   this->_length = clone._length;
}

void CompoundFileReader::CSInputStream::close(){
}

CompoundFileReader::CompoundFileReader(Directory* dir, char* name):
	entries(true,true)
{
   directory = dir;
   STRCPY_AtoA(fileName,name,CL_MAX_PATH);

   bool success = false;

   try {
      stream = dir->openInput(name);

      // read the directory and init files
      int32_t count = stream->readVInt();
      FileEntry* entry = NULL;
      TCHAR tid[CL_MAX_PATH];
      for (int32_t i=0; i<count; i++) {
            int64_t offset = stream->readLong();
            stream->readString(tid,CL_MAX_PATH);
            char* aid = STRDUP_TtoA(tid);

            if (entry != NULL) {
               // set length of the previous entry
               entry->length = offset - entry->offset;
            }

            entry = _CLNEW FileEntry();
            entry->offset = offset;
            entries.put(aid, entry);
      }

      // set the length of the final entry
      if (entry != NULL) {
            entry->length = stream->length() - entry->offset;
      }

      success = true;

   }_CLFINALLY(
      if (! success && (stream != NULL)) {
            try {
               stream->close();
               _CLDELETE(stream);
			   } catch (CLuceneError& err){
                if ( err.number() != CL_ERR_IO )
                    throw err;
				   //else ignore
            }
      }
   )
}

CompoundFileReader::~CompoundFileReader(){
	close();
}

Directory* CompoundFileReader::getDirectory(){
   return directory;
}

char* CompoundFileReader::getName(){
   return fileName;
}

void CompoundFileReader::close(){
  SCOPED_LOCK_MUTEX(THIS_LOCK)

  if (stream != NULL){
      entries.clear();
      stream->close();
      _CLDELETE(stream);
  }
}

IndexInput* CompoundFileReader::openInput(const char* id){
  SCOPED_LOCK_MUTEX(THIS_LOCK)

  if (stream == NULL)
      _CLTHROWA(CL_ERR_IO,"Stream closed");
	 
  const FileEntry* entry = entries.get(id);
  if (entry == NULL){
      char buf[CL_MAX_PATH+30];
      strcpy(buf,"No sub-file with id ");
      strncat(buf,id,CL_MAX_PATH);
      strcat(buf," found");
      _CLTHROWA(CL_ERR_IO,buf);
  }
  return _CLNEW CSInputStream(stream, entry->offset, entry->length);
}

char** CompoundFileReader::list() const{
  char** r = _CL_NEWARRAY(char*,entries.size()+1);
  int32_t j = 0;
  for ( CL_NS(util)::CLHashMap<const char*,CompoundFileReader::FileEntry*,Compare::Char>::const_iterator i=entries.begin();i!=entries.end();i++ ){
     r[j] = STRDUP_AtoA(i->first);
     j++;
  }
  r[entries.size()]=NULL;

  return r;
}

bool CompoundFileReader::fileExists(const char* name) const{
   return entries.exists(name);
}

int64_t CompoundFileReader::fileModified(const char* name) const{
  return directory->fileModified(fileName);
}

void CompoundFileReader::touchFile(const char* name){
  directory->touchFile(fileName);
}

void CompoundFileReader::deleteFile(const char* name, const bool throwError){
   _CLTHROWA(CL_ERR_UnsupportedOperation,"UnsupportedOperationException");
}

void CompoundFileReader::renameFile(const char* from, const char* to){
   _CLTHROWA(CL_ERR_UnsupportedOperation,"UnsupportedOperationException");
}

int64_t CompoundFileReader::fileLength(const char* name) const{
  FileEntry* e = entries.get(name);
  if (e == NULL){
     char buf[CL_MAX_PATH + 30];
     strcpy(buf,"File ");
     strncat(buf,name,CL_MAX_PATH );
     strcat(buf," does not exist");
  }
  return e->length;
}
IndexOutput* CompoundFileReader::createOutput(const char* name){
   _CLTHROWA(CL_ERR_UnsupportedOperation,"UnsupportedOperationException");
}
LuceneLock* CompoundFileReader::makeLock(const char* name){
   _CLTHROWA(CL_ERR_UnsupportedOperation,"UnsupportedOperationException");
}

TCHAR* CompoundFileReader::toString() const{
	TCHAR* ret = _CL_NEWARRAY(TCHAR,strlen(fileName)+20); //20=strlen("CompoundFileReader@")
	_tcscpy(ret,_T("CompoundFileReader@"));
	STRCPY_AtoT(ret+19,fileName,strlen(fileName));

	return ret;
}

CompoundFileWriter::CompoundFileWriter(Directory* dir, char* name):
	ids(true,false),entries(true){
  if (dir == NULL)
      _CLTHROWA(CL_ERR_IO,"Missing directory");
  if (name == NULL)
      _CLTHROWA(CL_ERR_IO,"Missing name");
  merged = false;
  directory = dir;
  strncpy(fileName,name,CL_MAX_PATH);
}

CompoundFileWriter::~CompoundFileWriter(){
}

Directory* CompoundFileWriter::getDirectory(){
  return directory;
}

/** Returns the name of the compound file. */
char* CompoundFileWriter::getName(){
  return fileName;
}

void CompoundFileWriter::addFile(char* file){
  if (merged)
      _CLTHROWA(CL_ERR_IO,"Can't add extensions after merge has been called");

  if (file == NULL)
      _CLTHROWA(CL_ERR_IO,"Missing source file");

  if (ids.exists(file)){
     char buf[CL_MAX_PATH + 30];
     strcpy(buf,"File ");
     strncat(buf,file,CL_MAX_PATH);
     strcat(buf," already added");
     _CLTHROWA(CL_ERR_IO,buf);
  }
  ids.put(STRDUP_AtoA(file),0);

  WriterFileEntry* entry = _CLNEW WriterFileEntry();
  STRCPY_AtoA(entry->file,file,CL_MAX_PATH);
  entries.push_back(entry);
}

void CompoundFileWriter::close(){
  if (merged)
      _CLTHROWA(CL_ERR_IO,"Merge already performed");

  if (entries.size()==0) //isEmpty()
      _CLTHROWA(CL_ERR_IO,"No entries to merge have been defined");

  merged = true;

  // open the compound stream
  IndexOutput* os = NULL;
  try {
      os = directory->createOutput(fileName);

      // Write the number of entries
      os->writeVInt(entries.size());

      // Write the directory with all offsets at 0.
      // Remember the positions of directory entries so that we can
      // adjust the offsets later
      { //msvc6 for scope fix
		  TCHAR tfile[CL_MAX_PATH];
		  for ( CLLinkedList<WriterFileEntry*>::iterator i=entries.begin();i!=entries.end();i++ ){
			  WriterFileEntry* fe = *i;
			  fe->directoryOffset = os->getFilePointer();
			  os->writeLong(0);    // for now
			  STRCPY_AtoT(tfile,fe->file,CL_MAX_PATH);
			  os->writeString(tfile,_tcslen(tfile));
		  }
	  }

      // Open the files and copy their data into the stream.
      // Remeber the locations of each file's data section.
      { //msvc6 for scope fix
		  int32_t bufferLength = 1024;
		  uint8_t buffer[1024];
		  for ( CL_NS(util)::CLLinkedList<WriterFileEntry*>::iterator i=entries.begin();i!=entries.end();i++ ){
			  WriterFileEntry* fe = *i;
			  fe->dataOffset = os->getFilePointer();
			  copyFile(fe, os, buffer, bufferLength);
		  }
	  }

	  { //msvc6 for scope fix
		  // Write the data offsets into the directory of the compound stream
		  for ( CLLinkedList<WriterFileEntry*>::iterator i=entries.begin();i!=entries.end();i++ ){
			  WriterFileEntry* fe = *i;
			  os->seek(fe->directoryOffset);
			  os->writeLong(fe->dataOffset);
		  }
	  }


  } _CLFINALLY (
	  if (os != NULL) try { os->close(); _CLDELETE(os); } catch (...) { }
  );
}


void CompoundFileWriter::copyFile(WriterFileEntry* source, IndexOutput* os, uint8_t* buffer, int32_t bufferLength){
  IndexInput* is = NULL;
  try {
      int64_t startPtr = os->getFilePointer();

      is = directory->openInput(source->file);
      int64_t length = is->length();
      int64_t remainder = length;
      int32_t chunk = bufferLength;

      while(remainder > 0) {
          int32_t len = (int32_t)min((int64_t)chunk, remainder);
          is->readBytes(buffer, 0, len);
          os->writeBytes(buffer, len);
          remainder -= len;
      }

      // Verify that remainder is 0
      if (remainder != 0){
         TCHAR buf[CL_MAX_PATH+100];
         _sntprintf(buf,CL_MAX_PATH+100,_T("Non-zero remainder length after copying")
          _T(": %d (id: %s, length: %d, buffer size: %d)"),
          remainder,source->file,length,chunk );
		 _CLTHROWT(CL_ERR_IO,buf);
      }

      // Verify that the output length diff is equal to original file
      int64_t endPtr = os->getFilePointer();
      int64_t diff = endPtr - startPtr;
      if (diff != length){
         TCHAR buf[100];
         _sntprintf(buf,100,_T("Difference in the output file offsets %d ")
            _T("does not match the original file length %d"),diff,length);
         _CLTHROWT(CL_ERR_IO,buf);
      }
  } _CLFINALLY (
     if (is != NULL){
        is->close();
        _CLDELETE(is);
     }
  );
}

CL_NS_END
