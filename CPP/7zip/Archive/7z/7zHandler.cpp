// 7zHandler.cpp

#include "StdAfx.h"

#include "../../../../C/CpuArch.h"

#include "../../../Common/ComTry.h"
#include "../../../Common/IntToString.h"

#ifndef __7Z_SET_PROPERTIES
#include "../../../Windows/System.h"
#endif

#include "../Common/ItemNameUtils.h"

#include "7zHandler.h"
#include "7zProperties.h"

#ifdef __7Z_SET_PROPERTIES
#ifdef EXTRACT_ONLY
#include "../Common/ParseProperties.h"
#endif
#endif

using namespace NWindows;

extern UString ConvertMethodIdToString(UInt64 id);

namespace NArchive {
namespace N7z {

CHandler::CHandler()
{
  _crcSize = 4;

  #ifndef _NO_CRYPTO
  _passwordIsDefined = false;
  #endif

  #ifdef EXTRACT_ONLY
  #ifdef __7Z_SET_PROPERTIES
  _numThreads = NSystem::GetNumberOfProcessors();
  #endif
  #else
  Init();
  #endif
}

STDMETHODIMP CHandler::GetNumberOfItems(UInt32 *numItems)
{
  *numItems = _db.Files.Size();
  return S_OK;
}

// todo: move elsewhere
// 

class PathParser
{
private:
	const UString* path;
	int slashpos, nextpos;

public:
	// path should remain valid for duration of this objects lifetime
	PathParser(const UString* path) : path(path), slashpos(0), nextpos(-1)
	{
	}

	// Find the next directory in the path (starting from the root)
	// Returns false if there is no directory left.
	bool GetNextDirectory(UString& dir)
	{
		if (slashpos == -1) return false;

		bool has_next = true;
		nextpos = path->Find('/', slashpos);
		if (nextpos == -1) {
			nextpos = path->Length();
			has_next = false;				
		} 
		if (nextpos == slashpos) return false;
		dir = path->Mid(slashpos, nextpos-slashpos);

		slashpos = has_next ? nextpos + 1 : -1;
		return true;
	}
};

class CSzTree /*: public CSzTree*/
{
private:
	CRecordVector<CFolder*> blocks; // not owned
	CRecordVector<CSzTree*> leaves; // owned by this obj
	UString key;

	bool UpdateIfEmpty(const UString& key)
	{
		if (IsEmpty()) {
			this->key = key;
			return true;
		}
		return false;
	}

	// Checks if the directory exists AT THIS LEVEL only.
	bool FindDirectory(const UString& dir, CSzTree** found)
	{
		// int indx = leaves.FindInSorted(tofind);
		// if (indx == -1) {*found = 0; return false;} 
		for (size_t x = 0; x < leaves.Size(); x++) {
			if (!leaves[x]->key.Compare(dir)){
				*found = leaves[x];
				return true;
			}
		}
		return false;
	}

	CSzTree& AddDirCheckExist(const UString& key)
	{
		CSzTree* found;
		if (FindDirectory(key, &found)) return *found;
		leaves.Add(new CSzTree(key));
		return *leaves.Back();
	}

	CSzTree& AddSimpleDirectory(const UString& key)
	{
		if (UpdateIfEmpty(key)) return *this;
		return AddDirCheckExist(key);
	}

protected:

public:
	CSzTree(const UString& key) : key(key) {}

	virtual ~CSzTree() {
		for (int i = 0; i < leaves.Size(); i++)
			delete leaves[i];
		leaves.Clear();
	}

	bool IsEmpty() const 
	{
		return key.Length() == 0;
	}

	// Add a directory which may contain other directories, which should also 
	// get created. 
	// The value returned is the deepest nested directory created (ie where
	// a file in this path should go).
	CSzTree& AddDirectory(const UString& path)
	{
		PathParser p(&path);
		CSzTree* leaf = this;
		UString dir;
		while (p.GetNextDirectory(dir))
			leaf = &leaf->AddSimpleDirectory(dir);
		return *leaf;
	}

	// Find a directory relative to 'this'
	bool FindRelativeDirectory(const UString& relative_dir, CSzTree** out)
	{
		PathParser p(&relative_dir);
		CSzTree* leaf = this;
		UString dir;
		while (p.GetNextDirectory(dir))
		{
			if (!leaf->FindDirectory(dir, &leaf)) return false;
		}
		return true;
	}

	void PreorderPrint(int depth = 0)
	{
		UString prefix;
		for (int i = 0; i < depth; i++)
			prefix += L" ";

		wprintf(L"%s%s [%i blocks]\n", prefix.GetBuffer(), key.GetBuffer(),
			blocks.Size());

		for (size_t x = 0; x < leaves.Size(); x++)
			leaves[x]->PreorderPrint(depth+1);
	}

	// the folder should out live the tree
	void AddBlock(CFolder* folder)
	{
		blocks.AddToUniqueSorted(folder);
	}
};

// Explode the database into one database per folder.
void CHandler::Explode(CObjectVector<CArchiveDatabase>& exploded,
	CRecordVector<UInt64>& folderSizes, 
	CRecordVector<UInt64>& folderPositions)
{
	wprintf(L"Archive has %i blocks\n", _db.Folders.Size());
	// Parse the archive into its directory tree and associate folders (blocks)
	// with the correct level.
	CSzTree archiveStructure(L"/");
	for (int x = 0; x < _db.Files.Size(); x++)
	{
		CFileItem file = _db.Files[x];		
		UString dir = L"/";

		if (file.IsDir) dir = file.Name;
		else {
			int last = file.Name.ReverseFind(L'/');
			if (last != -1) dir = file.Name.Left(last);	
		}

		//wprintf(L"Adding directory %s\n", dir.GetBuffer());
		 
		CSzTree& structuredDir = archiveStructure.AddDirectory(dir);
		CFolder* folder = &_db.Folders[_db.FileIndexToFolderIndexMap[x]];
		if (!file.IsDir) structuredDir.AddBlock(folder);
	}
	archiveStructure.PreorderPrint();
	// all files in a block are in the same directory level
	// /folder/a/b/c/datahere
	// so to find a block's level, find a file in that block and get its
	// level.
	// If the level exceeds a depth then all sub folders are put into that
	// archive.
	// So, it'd make sense to parse the structure into a directory tree
	for (int folderIndex = 0; folderIndex < _db.Folders.Size(); folderIndex++)
	{
		CFolder& folder = _db.Folders[folderIndex];
		folderSizes.Add(_db.GetFolderFullPackSize(folderIndex));
		folderPositions.Add(_db.GetFolderStreamPos(folderIndex, 0));

		CArchiveDatabase newDatabase;
		newDatabase.Folders.Add(folder); // not copy constructed

		newDatabase.NumUnpackStreamsVector.Add(
			_db.NumUnpackStreamsVector[folderIndex]);

		// i think this is right
		for (int packSizes = 0; packSizes < folder.PackStreams.Size(); packSizes++)
			newDatabase.PackSizes.Add(_db.GetFolderPackStreamSize(folderIndex, packSizes));

		//newDatabase.PackSizes.Add(folderLen); 
		newDatabase.PackCRCs.Add(folder.UnpackCRC);
		newDatabase.PackCRCsDefined.Add(folder.UnpackCRCDefined);

		for (int x = 0; x < _db.Files.Size(); x++) { // should just do this once per db, not folder
			UInt64 _folderIndex = _db.FileIndexToFolderIndexMap[x];
			if (_folderIndex == folderIndex) {
				CFileItem file;
				CFileItem2 finfo;
				_db.GetFile(x, file, finfo);
				newDatabase.AddFile(file, finfo);
			}
		}
		exploded.Add(newDatabase); // copy constructed
	}
}
#ifdef _SFX

IMP_IInArchive_ArcProps_NO

STDMETHODIMP CHandler::GetNumberOfProperties(UInt32 * /* numProperties */)
{
  return E_NOTIMPL;
}

STDMETHODIMP CHandler::GetPropertyInfo(UInt32 /* index */,
      BSTR * /* name */, PROPID * /* propID */, VARTYPE * /* varType */)
{
  return E_NOTIMPL;
}


#else

STATPROPSTG kArcProps[] =
{
  { NULL, kpidMethod, VT_BSTR},
  { NULL, kpidSolid, VT_BOOL},
  { NULL, kpidNumBlocks, VT_UI4},
  { NULL, kpidPhySize, VT_UI8},
  { NULL, kpidHeadersSize, VT_UI8},
  { NULL, kpidOffset, VT_UI8}
};

STDMETHODIMP CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value)
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  switch(propID)
  {
    case kpidMethod:
    {
      UString resString;
      CRecordVector<UInt64> ids;
      int i;
      for (i = 0; i < _db.Folders.Size(); i++)
      {
        const CFolder &f = _db.Folders[i];
        for (int j = f.Coders.Size() - 1; j >= 0; j--)
          ids.AddToUniqueSorted(f.Coders[j].MethodID);
      }

      for (i = 0; i < ids.Size(); i++)
      {
        UInt64 id = ids[i];
        UString methodName;
        /* bool methodIsKnown = */ FindMethod(EXTERNAL_CODECS_VARS id, methodName);
        if (methodName.IsEmpty())
          methodName = ConvertMethodIdToString(id);
        if (!resString.IsEmpty())
          resString += L' ';
        resString += methodName;
      }
      prop = resString;
      break;
    }
    case kpidSolid: prop = _db.IsSolid(); break;
    case kpidNumBlocks: prop = (UInt32)_db.Folders.Size(); break;
    case kpidHeadersSize:  prop = _db.HeadersSize; break;
    case kpidPhySize:  prop = _db.PhySize; break;
    case kpidOffset: if (_db.ArchiveInfo.StartPosition != 0) prop = _db.ArchiveInfo.StartPosition; break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

IMP_IInArchive_ArcProps

#endif

static void SetPropFromUInt64Def(CUInt64DefVector &v, int index, NCOM::CPropVariant &prop)
{
  UInt64 value;
  if (v.GetItem(index, value))
  {
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)value;
    ft.dwHighDateTime = (DWORD)(value >> 32);
    prop = ft;
  }
}

#ifndef _SFX

static UString ConvertUInt32ToString(UInt32 value)
{
  wchar_t buffer[32];
  ConvertUInt64ToString(value, buffer);
  return buffer;
}

static UString GetStringForSizeValue(UInt32 value)
{
  for (int i = 31; i >= 0; i--)
    if ((UInt32(1) << i) == value)
      return ConvertUInt32ToString(i);
  UString result;
  if (value % (1 << 20) == 0)
  {
    result += ConvertUInt32ToString(value >> 20);
    result += L"m";
  }
  else if (value % (1 << 10) == 0)
  {
    result += ConvertUInt32ToString(value >> 10);
    result += L"k";
  }
  else
  {
    result += ConvertUInt32ToString(value);
    result += L"b";
  }
  return result;
}

static const UInt64 k_Copy = 0x0;
static const UInt64 k_Delta = 3;
static const UInt64 k_LZMA2 = 0x21;
static const UInt64 k_LZMA  = 0x030101;
static const UInt64 k_PPMD  = 0x030401;

static wchar_t GetHex(Byte value)
{
  return (wchar_t)((value < 10) ? (L'0' + value) : (L'A' + (value - 10)));
}
static inline void AddHexToString(UString &res, Byte value)
{
  res += GetHex((Byte)(value >> 4));
  res += GetHex((Byte)(value & 0xF));
}

#endif

bool CHandler::IsEncrypted(UInt32 index2) const
{
  CNum folderIndex = _db.FileIndexToFolderIndexMap[index2];
  if (folderIndex != kNumNoIndex)
    return _db.Folders[folderIndex].IsEncrypted();
  return false;
}

STDMETHODIMP CHandler::GetProperty(UInt32 index, PROPID propID,  PROPVARIANT *value)
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  
  /*
  const CRef2 &ref2 = _refs[index];
  if (ref2.Refs.IsEmpty())
    return E_FAIL;
  const CRef &ref = ref2.Refs.Front();
  */
  
  const CFileItem &item = _db.Files[index];
  UInt32 index2 = index;

  switch(propID)
  {
    case kpidPath:
      if (!item.Name.IsEmpty())
        prop = NItemName::GetOSName(item.Name);
      break;
    case kpidIsDir:  prop = item.IsDir; break;
    case kpidSize:
    {
      prop = item.Size;
      // prop = ref2.Size;
      break;
    }
    case kpidPackSize:
    {
      // prop = ref2.PackSize;
      {
        CNum folderIndex = _db.FileIndexToFolderIndexMap[index2];
        if (folderIndex != kNumNoIndex)
        {
          if (_db.FolderStartFileIndex[folderIndex] == (CNum)index2)
            prop = _db.GetFolderFullPackSize(folderIndex);
          /*
          else
            prop = (UInt64)0;
          */
        }
        else
          prop = (UInt64)0;
      }
      break;
    }
    case kpidPosition:  { UInt64 v; if (_db.StartPos.GetItem(index2, v)) prop = v; break; }
    case kpidCTime:  SetPropFromUInt64Def(_db.CTime, index2, prop); break;
    case kpidATime:  SetPropFromUInt64Def(_db.ATime, index2, prop); break;
    case kpidMTime:  SetPropFromUInt64Def(_db.MTime, index2, prop); break;
    case kpidAttrib:  if (item.AttribDefined) prop = item.Attrib; break;
    case kpidCRC:  if (item.CrcDefined) prop = item.Crc; break;
    case kpidEncrypted:  prop = IsEncrypted(index2); break;
    case kpidIsAnti:  prop = _db.IsItemAnti(index2); break;
    #ifndef _SFX
    case kpidMethod:
      {
        CNum folderIndex = _db.FileIndexToFolderIndexMap[index2];
        if (folderIndex != kNumNoIndex)
        {
          const CFolder &folderInfo = _db.Folders[folderIndex];
          UString methodsString;
          for (int i = folderInfo.Coders.Size() - 1; i >= 0; i--)
          {
            const CCoderInfo &coder = folderInfo.Coders[i];
            if (!methodsString.IsEmpty())
              methodsString += L' ';

            UString methodName, propsString;
            bool methodIsKnown = FindMethod(
              EXTERNAL_CODECS_VARS
              coder.MethodID, methodName);
            
            if (!methodIsKnown)
              methodsString += ConvertMethodIdToString(coder.MethodID);
            else
            {
              methodsString += methodName;
              if (coder.MethodID == k_Delta && coder.Props.GetCapacity() == 1)
                propsString = ConvertUInt32ToString((UInt32)coder.Props[0] + 1);
              else if (coder.MethodID == k_LZMA && coder.Props.GetCapacity() == 5)
              {
                UInt32 dicSize = GetUi32((const Byte *)coder.Props + 1);
                propsString = GetStringForSizeValue(dicSize);
              }
              else if (coder.MethodID == k_LZMA2 && coder.Props.GetCapacity() == 1)
              {
                Byte p = coder.Props[0];
                UInt32 dicSize = (((UInt32)2 | ((p) & 1)) << ((p) / 2 + 11));
                propsString = GetStringForSizeValue(dicSize);
              }
              else if (coder.MethodID == k_PPMD && coder.Props.GetCapacity() == 5)
              {
                Byte order = *(const Byte *)coder.Props;
                propsString = L'o';
                propsString += ConvertUInt32ToString(order);
                propsString += L":mem";
                UInt32 dicSize = GetUi32((const Byte *)coder.Props + 1);
                propsString += GetStringForSizeValue(dicSize);
              }
              else if (coder.MethodID == k_AES && coder.Props.GetCapacity() >= 1)
              {
                const Byte *data = (const Byte *)coder.Props;
                Byte firstByte = *data++;
                UInt32 numCyclesPower = firstByte & 0x3F;
                propsString = ConvertUInt32ToString(numCyclesPower);
                /*
                if ((firstByte & 0xC0) != 0)
                {
                  UInt32 saltSize = (firstByte >> 7) & 1;
                  UInt32 ivSize = (firstByte >> 6) & 1;
                  if (coder.Props.GetCapacity() >= 2)
                  {
                    Byte secondByte = *data++;
                    saltSize += (secondByte >> 4);
                    ivSize += (secondByte & 0x0F);
                  }
                }
                */
              }
            }
            if (!propsString.IsEmpty())
            {
              methodsString += L':';
              methodsString += propsString;
            }
            else if (coder.Props.GetCapacity() > 0)
            {
              methodsString += L":[";
              for (size_t bi = 0; bi < coder.Props.GetCapacity(); bi++)
              {
                if (bi > 5 && bi + 1 < coder.Props.GetCapacity())
                {
                  methodsString += L"..";
                  break;
                }
                else
                  AddHexToString(methodsString, coder.Props[bi]);
              }
              methodsString += L']';
            }
          }
          prop = methodsString;
        }
      }
      break;
    case kpidBlock:
      {
        CNum folderIndex = _db.FileIndexToFolderIndexMap[index2];
        if (folderIndex != kNumNoIndex)
          prop = (UInt32)folderIndex;
      }
      break;
    case kpidPackedSize0:
    case kpidPackedSize1:
    case kpidPackedSize2:
    case kpidPackedSize3:
    case kpidPackedSize4:
      {
        CNum folderIndex = _db.FileIndexToFolderIndexMap[index2];
        if (folderIndex != kNumNoIndex)
        {
          const CFolder &folderInfo = _db.Folders[folderIndex];
          if (_db.FolderStartFileIndex[folderIndex] == (CNum)index2 &&
              folderInfo.PackStreams.Size() > (int)(propID - kpidPackedSize0))
          {
            prop = _db.GetFolderPackStreamSize(folderIndex, propID - kpidPackedSize0);
          }
          else
            prop = (UInt64)0;
        }
        else
          prop = (UInt64)0;
      }
      break;
    #endif
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

STDMETHODIMP CHandler::Open(IInStream *stream,
    const UInt64 *maxCheckStartPosition,
    IArchiveOpenCallback *openArchiveCallback)
{
  COM_TRY_BEGIN
  Close();
  #ifndef _SFX
  _fileInfoPopIDs.Clear();
  #endif
  try
  {
    CMyComPtr<IArchiveOpenCallback> openArchiveCallbackTemp = openArchiveCallback;

    #ifndef _NO_CRYPTO
    CMyComPtr<ICryptoGetTextPassword> getTextPassword;
    if (openArchiveCallback)
    {
      openArchiveCallbackTemp.QueryInterface(
          IID_ICryptoGetTextPassword, &getTextPassword);
    }
    #endif
    CInArchive archive;
    RINOK(archive.Open(stream, maxCheckStartPosition));
    #ifndef _NO_CRYPTO
    _passwordIsDefined = false;
    UString password;
    #endif
    HRESULT result = archive.ReadDatabase(
      EXTERNAL_CODECS_VARS
      _db
      #ifndef _NO_CRYPTO
      , getTextPassword, _passwordIsDefined
      #endif
      );
    RINOK(result);
    _db.Fill();
    _inStream = stream;
  }
  catch(...)
  {
    Close();
    return S_FALSE;
  }
  // _inStream = stream;
  #ifndef _SFX
  FillPopIDs();
  #endif
  return S_OK;
  COM_TRY_END
}

STDMETHODIMP CHandler::Close()
{
  COM_TRY_BEGIN
  _inStream.Release();
  _db.Clear();
  return S_OK;
  COM_TRY_END
}

#ifdef __7Z_SET_PROPERTIES
#ifdef EXTRACT_ONLY

STDMETHODIMP CHandler::SetProperties(const wchar_t **names, const PROPVARIANT *values, Int32 numProperties)
{
  COM_TRY_BEGIN
  const UInt32 numProcessors = NSystem::GetNumberOfProcessors();
  _numThreads = numProcessors;

  for (int i = 0; i < numProperties; i++)
  {
    UString name = names[i];
    name.MakeUpper();
    if (name.IsEmpty())
      return E_INVALIDARG;
    const PROPVARIANT &value = values[i];
    UInt32 number;
    int index = ParseStringToUInt32(name, number);
    if (index == 0)
    {
      if(name.Left(2).CompareNoCase(L"MT") == 0)
      {
        RINOK(ParseMtProp(name.Mid(2), value, numProcessors, _numThreads));
        continue;
      }
      else
        return E_INVALIDARG;
    }
  }
  return S_OK;
  COM_TRY_END
}

#endif
#endif

IMPL_ISetCompressCodecsInfo

}}
