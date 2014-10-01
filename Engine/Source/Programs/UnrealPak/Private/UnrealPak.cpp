// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "UnrealPak.h"
#include "RequiredProgramMainCPPInclude.h"
#include "IPlatformFilePak.h"
#include "SecureHash.h"
#include "BigInt.h"
#include "SignedArchiveWriter.h"
#include "KeyGenerator.h"
#include "AES.h"
#include "UniquePtr.h"

IMPLEMENT_APPLICATION(UnrealPak, "UnrealPak");

struct FPakCommandLineParameters
{
	FPakCommandLineParameters()
		: CompressionBlockSize(64*1024)
		, FileSystemBlockSize(0)
	{}

	int32  CompressionBlockSize;
	int64  FileSystemBlockSize;
};

struct FPakEntryPair
{
	FString Filename;
	FPakEntry Info;
};

struct FPakInputPair
{
	FString Source;
	FString Dest;
	bool bNeedsCompression;
	bool bNeedEncryption;

	FPakInputPair()
		: bNeedsCompression(false)
		, bNeedEncryption(false)
	{}

	FPakInputPair(const FString& InSource, const FString& InDest)
		: Source(InSource)
		, Dest(InDest)
		, bNeedsCompression(false)
		, bNeedEncryption(false)
	{}

	FORCEINLINE bool operator==(const FPakInputPair& Other) const
	{
		return Source == Other.Source;
	}
};

struct FPakEntryOrder
{
	FPakEntryOrder() : Order(MAX_uint64) {}
	FString Filename;
	uint64  Order;
};

struct FCompressedFileBuffer
{
	FCompressedFileBuffer()
		: OriginalSize(0)
		,TotalCompressedSize(0)
		,FileCompressionBlockSize(0)
		,CompressedBufferSize(0)
	{

	}

	void Reinitialize(FArchive* File,ECompressionFlags CompressionMethod,int64 CompressionBlockSize)
	{
		OriginalSize = File->TotalSize();
		TotalCompressedSize = 0;
		FileCompressionBlockSize = 0;
		FileCompressionMethod = CompressionMethod;
		CompressedBlocks.Reset();
		CompressedBlocks.AddUninitialized((OriginalSize+CompressionBlockSize-1)/CompressionBlockSize);
	}

	void EnsureBufferSpace(int64 RequiredSpace)
	{
		if(RequiredSpace > CompressedBufferSize)
		{
			uint8* NewPtr = (uint8*)FMemory::Malloc(RequiredSpace);
			FMemory::Memcpy(NewPtr,CompressedBuffer.Get(),CompressedBufferSize);
			CompressedBuffer.Reset(NewPtr);
			CompressedBufferSize = RequiredSpace;
		}
	}

	bool CompressFileToWorkingBuffer(const FPakInputPair& InFile,uint8*& InOutPersistentBuffer,int64& InOutBufferSize,ECompressionFlags CompressionMethod,const int32 CompressionBlockSize);

	int64						 OriginalSize;
	int64						 TotalCompressedSize;
	int32						 FileCompressionBlockSize;
	ECompressionFlags			 FileCompressionMethod;
	TArray<FPakCompressedBlock>  CompressedBlocks;
	int64						 CompressedBufferSize;
	TUniquePtr<uint8>		     CompressedBuffer;
};

FString GetLongestPath(TArray<FPakInputPair>& FilesToAdd)
{
	FString LongestPath;
	int32 MaxNumDirectories = 0;

	for (int32 FileIndex = 0; FileIndex < FilesToAdd.Num(); FileIndex++)
	{
		FString& Filename = FilesToAdd[FileIndex].Dest;
		int32 NumDirectories = 0;
		for (int32 Index = 0; Index < Filename.Len(); Index++)
		{
			if (Filename[Index] == '/')
			{
				NumDirectories++;
			}
		}
		if (NumDirectories > MaxNumDirectories)
		{
			LongestPath = Filename;
			MaxNumDirectories = NumDirectories;
		}
	}
	return FPaths::GetPath(LongestPath) + TEXT("/");
}

FString GetCommonRootPath(TArray<FPakInputPair>& FilesToAdd)
{
	FString Root = GetLongestPath(FilesToAdd);
	for (int32 FileIndex = 0; FileIndex < FilesToAdd.Num() && Root.Len(); FileIndex++)
	{
		FString Filename(FilesToAdd[FileIndex].Dest);
		FString Path = FPaths::GetPath(Filename) + TEXT("/");
		int32 CommonSeparatorIndex = -1;
		int32 SeparatorIndex = Path.Find(TEXT("/"), ESearchCase::CaseSensitive);
		while (SeparatorIndex >= 0)
		{
			if (FCString::Strnicmp(*Root, *Path, SeparatorIndex + 1) != 0)
			{
				break;
			}
			CommonSeparatorIndex = SeparatorIndex;
			if (CommonSeparatorIndex + 1 < Path.Len())
			{
				SeparatorIndex = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, CommonSeparatorIndex + 1);
			}
			else
			{
				break;
			}
		}
		if ((CommonSeparatorIndex + 1) < Root.Len())
		{
			Root = Root.Mid(0, CommonSeparatorIndex + 1);
		}
	}
	return Root;
}

bool FCompressedFileBuffer::CompressFileToWorkingBuffer(const FPakInputPair& InFile, uint8*& InOutPersistentBuffer, int64& InOutBufferSize, ECompressionFlags CompressionMethod, const int32 CompressionBlockSize)
{
	TAutoPtr<FArchive> FileHandle(IFileManager::Get().CreateFileReader(*InFile.Source));
	if(!FileHandle.IsValid())
	{
		TotalCompressedSize = 0;
		return false;
	}

	Reinitialize(FileHandle.GetOwnedPointer(), CompressionMethod, CompressionBlockSize);
	const int64 FileSize = OriginalSize;
	const int64 PaddedEncryptedFileSize = Align(FileSize,FAES::AESBlockSize);
	if(InOutBufferSize < PaddedEncryptedFileSize)
	{
		InOutPersistentBuffer = (uint8*)FMemory::Realloc(InOutPersistentBuffer,PaddedEncryptedFileSize);
		InOutBufferSize = FileSize;
	}

	// Load to buffer
	FileHandle->Serialize(InOutPersistentBuffer,FileSize);

	// Build buffers for working
	int64 UncompressedSize = FileSize;
	int32 CompressionBufferSize = Align(FCompression::CompressMemoryBound(CompressionMethod,CompressionBlockSize),FAES::AESBlockSize);
	EnsureBufferSpace(Align(FCompression::CompressMemoryBound(CompressionMethod,FileSize),FAES::AESBlockSize));


	TotalCompressedSize = 0;
	int64 UncompressedBytes = 0;
	int32 CurrentBlock = 0;
	while(UncompressedSize)
	{
		int32 BlockSize = (int32)FMath::Min<int64>(UncompressedSize,CompressionBlockSize);
		int32 CompressedBlockSize = CompressionBufferSize;
		FileCompressionBlockSize = FMath::Max<uint32>(BlockSize, FileCompressionBlockSize);
		EnsureBufferSpace(TotalCompressedSize+CompressedBlockSize);
		if(!FCompression::CompressMemory(CompressionMethod,CompressedBuffer.Get()+TotalCompressedSize,CompressedBlockSize,InOutPersistentBuffer+UncompressedBytes,BlockSize))
		{
			return false;
		}
		UncompressedSize -= BlockSize;
		UncompressedBytes += BlockSize;

		CompressedBlocks[CurrentBlock].CompressedStart = TotalCompressedSize;
		CompressedBlocks[CurrentBlock].CompressedEnd = TotalCompressedSize+CompressedBlockSize;
		++CurrentBlock;

		TotalCompressedSize += CompressedBlockSize;

		if(InFile.bNeedEncryption)
		{
			int32 EncryptionBlockPadding = Align(TotalCompressedSize,FAES::AESBlockSize);
			for(int64 FillIndex=TotalCompressedSize; FillIndex < EncryptionBlockPadding; ++FillIndex)
			{
				// Fill the trailing buffer with random bytes from file
				CompressedBuffer.Get()[FillIndex] = CompressedBuffer.Get()[rand()%TotalCompressedSize];
			}
			TotalCompressedSize += EncryptionBlockPadding - TotalCompressedSize;
		}
	}

	return true;
}

bool CopyFileToPak(FArchive& InPak, const FString& InMountPoint, const FPakInputPair& InFile, uint8*& InOutPersistentBuffer, int64& InOutBufferSize, FPakEntryPair& OutNewEntry)
{	
	TAutoPtr<FArchive> FileHandle(IFileManager::Get().CreateFileReader(*InFile.Source));
	bool bFileExists = FileHandle.IsValid();
	if (bFileExists)
	{
		const int64 FileSize = FileHandle->TotalSize();
		const int64 PaddedEncryptedFileSize = Align(FileSize, FAES::AESBlockSize); 
		OutNewEntry.Filename = InFile.Dest.Mid(InMountPoint.Len());
		OutNewEntry.Info.Offset = 0; // Don't serialize offsets here.
		OutNewEntry.Info.Size = FileSize;
		OutNewEntry.Info.UncompressedSize = FileSize;
		OutNewEntry.Info.CompressionMethod = COMPRESS_None;
		OutNewEntry.Info.bEncrypted = InFile.bNeedEncryption;

		if (InOutBufferSize < PaddedEncryptedFileSize)
		{
			InOutPersistentBuffer = (uint8*)FMemory::Realloc(InOutPersistentBuffer, PaddedEncryptedFileSize);
			InOutBufferSize = FileSize;
		}

		// Load to buffer
		FileHandle->Serialize(InOutPersistentBuffer, FileSize);

		{
			int64 SizeToWrite = FileSize;
			if (InFile.bNeedEncryption)
			{
				for(int64 FillIndex=FileSize; FillIndex < PaddedEncryptedFileSize && InFile.bNeedEncryption; ++FillIndex)
				{
					// Fill the trailing buffer with random bytes from file
					InOutPersistentBuffer[FillIndex] = InOutPersistentBuffer[rand()%FileSize];
				}

				//Encrypt the buffer before writing it to disk
				FAES::EncryptData(InOutPersistentBuffer, PaddedEncryptedFileSize);
				// Update the size to be written
				SizeToWrite = PaddedEncryptedFileSize;
				OutNewEntry.Info.bEncrypted = true;
			}

			// Calculate the buffer hash value
			FSHA1::HashBuffer(InOutPersistentBuffer,FileSize,OutNewEntry.Info.Hash);

			// Write to file
			OutNewEntry.Info.Serialize(InPak,FPakInfo::PakFile_Version_Latest);
			InPak.Serialize(InOutPersistentBuffer,SizeToWrite);
		}
	}
	return bFileExists;
}

bool CopyCompressedFileToPak(FArchive& InPak, const FString& InMountPoint, const FPakInputPair& InFile, const FCompressedFileBuffer& CompressedFile, FPakEntryPair& OutNewEntry)
{
	if (CompressedFile.TotalCompressedSize == 0)
	{
		return false;
	}

	int64 HeaderTell = InPak.Tell();
	OutNewEntry.Info.CompressionMethod = CompressedFile.FileCompressionMethod;
	OutNewEntry.Info.CompressionBlocks.AddUninitialized(CompressedFile.CompressedBlocks.Num());

	int64 TellPos = InPak.Tell() + OutNewEntry.Info.GetSerializedSize(FPakInfo::PakFile_Version_Latest);
	const TArray<FPakCompressedBlock>& Blocks = CompressedFile.CompressedBlocks;
	for (int32 BlockIndex = 0, BlockCount = CompressedFile.CompressedBlocks.Num(); BlockIndex < BlockCount; ++BlockIndex)
	{
		OutNewEntry.Info.CompressionBlocks[BlockIndex].CompressedStart = Blocks[BlockIndex].CompressedStart + TellPos;
		OutNewEntry.Info.CompressionBlocks[BlockIndex].CompressedEnd = Blocks[BlockIndex].CompressedEnd + TellPos;
	}

	if (InFile.bNeedEncryption)
	{
		FAES::EncryptData(CompressedFile.CompressedBuffer.Get(), CompressedFile.TotalCompressedSize);
	}

	//Hash the final buffer thats written
	FSHA1 Hash;
	Hash.Update(CompressedFile.CompressedBuffer.Get(), CompressedFile.TotalCompressedSize);
	Hash.Final();

	// Update file size & Hash
	OutNewEntry.Info.CompressionBlockSize = CompressedFile.FileCompressionBlockSize;
	OutNewEntry.Info.UncompressedSize = CompressedFile.OriginalSize;
	OutNewEntry.Info.Size = CompressedFile.TotalCompressedSize;
	Hash.GetHash(OutNewEntry.Info.Hash);

	//	Write the header, then the data
	OutNewEntry.Filename = InFile.Dest.Mid(InMountPoint.Len());
	OutNewEntry.Info.Offset = 0; // Don't serialize offsets here.
	OutNewEntry.Info.bEncrypted = InFile.bNeedEncryption;
	OutNewEntry.Info.Serialize(InPak,FPakInfo::PakFile_Version_Latest);
	InPak.Serialize(CompressedFile.CompressedBuffer.Get(), CompressedFile.TotalCompressedSize);

	return true;
}

void ProcessOrderFile(int32 ArgC, TCHAR* ArgV[], TMap<FString, uint64>& OrderMap)
{
	// List of all items to add to pak file
	FString ResponseFile;
	if (FParse::Value(FCommandLine::Get(), TEXT("-order="), ResponseFile))
	{
		FString Text;
		UE_LOG(LogPakFile, Display, TEXT("Loading pak order file %s..."), *ResponseFile);
		if (FFileHelper::LoadFileToString(Text, *ResponseFile))
		{
			// Read all lines
			TArray<FString> Lines;
			Text.ParseIntoArray(&Lines, TEXT("\n"), true);
			for (int32 EntryIndex = 0; EntryIndex < Lines.Num(); EntryIndex++)
			{
				Lines[EntryIndex].ReplaceInline(TEXT("\r"), TEXT(""));
				Lines[EntryIndex].ReplaceInline(TEXT("\n"), TEXT(""));
				int32 OpenOrderNumber = EntryIndex;
				if (Lines[EntryIndex].FindLastChar('"', OpenOrderNumber))
				{
					FString ReadNum = Lines[EntryIndex].RightChop(OpenOrderNumber+1);
					Lines[EntryIndex] = Lines[EntryIndex].Left(OpenOrderNumber+1);
					ReadNum.Trim();
					if (ReadNum.IsNumeric())
					{
						OpenOrderNumber = FCString::Atoi(*ReadNum);
					}
				}
				Lines[EntryIndex] = Lines[EntryIndex].TrimQuotes();
				FString Path=FString::Printf(TEXT("%s"), *Lines[EntryIndex]);
				FPaths::NormalizeFilename(Path);
				OrderMap.Add(Path, OpenOrderNumber);
			}
			UE_LOG(LogPakFile, Display, TEXT("Finished loading pak order file %s."), *ResponseFile);
		}
		else 
		{
			UE_LOG(LogPakFile, Display, TEXT("Unable to load pak order file %s."), *ResponseFile);
		}
	}
}

static void CommandLineParseHelper(const TCHAR* InCmdLine, TArray<FString>& Tokens, TArray<FString>& Switches)
{
	FString NextToken;
	while(FParse::Token(InCmdLine,NextToken,false))
	{
		if((**NextToken == TCHAR('-')))
		{
			new(Switches)FString(NextToken.Mid(1));
		}
		else
		{
			new(Tokens)FString(NextToken);
		}
	}
}

void ProcessCommandLine(int32 ArgC, TCHAR* ArgV[], TArray<FPakInputPair>& Entries, FPakCommandLineParameters& CmdLineParameters)
{
	// List of all items to add to pak file
	FString ResponseFile;
	FString ClusterSizeString;

	if (FParse::Value(FCommandLine::Get(), TEXT("-blocksize="), ClusterSizeString) && 
		FParse::Value(FCommandLine::Get(), TEXT("-blocksize="), CmdLineParameters.FileSystemBlockSize))
	{
		if (ClusterSizeString.EndsWith(TEXT("MB")))
		{
			CmdLineParameters.FileSystemBlockSize *= 1024*1024;
		}
		else if (ClusterSizeString.EndsWith(TEXT("KB")))
		{
			CmdLineParameters.FileSystemBlockSize *= 1024;
		}
	}
	else
	{
		CmdLineParameters.FileSystemBlockSize = 0;
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("-create="), ResponseFile))
	{
		bool bCompress = false;
		bool bEncrypt = false;
		TArray<FString> Lines;

		if (FParse::Param(FCommandLine::Get(), TEXT("compress")))
		{
			bCompress = true;
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("encrypt")))
		{
			bEncrypt = true;
		}

		if (IFileManager::Get().DirectoryExists(*ResponseFile))
		{
			IFileManager::Get().FindFilesRecursive(Lines, *ResponseFile, TEXT("*"), true, false);
		}
		else
		{
			FString Text;
			UE_LOG(LogPakFile, Display, TEXT("Loading response file %s"), *ResponseFile);
			if (FFileHelper::LoadFileToString(Text, *ResponseFile))
			{
				// Remove all carriage return characters.
				Text.ReplaceInline(TEXT("\r"), TEXT(""));
				// Read all lines
				Text.ParseIntoArray(&Lines, TEXT("\n"), true);
			}
			else
			{
				UE_LOG(LogPakFile, Error, TEXT("Failed to load %s"), *ResponseFile);
			}
		}

		for (int32 EntryIndex = 0; EntryIndex < Lines.Num(); EntryIndex++)
		{
			TArray<FString> SourceAndDest;
			TArray<FString> Switches;
			CommandLineParseHelper(*Lines[EntryIndex], SourceAndDest, Switches);
			FPakInputPair Input;

			Input.Source = SourceAndDest[0];
			FPaths::NormalizeFilename(Input.Source);
			if (SourceAndDest.Num() > 1)
			{
				Input.Dest = FPaths::GetPath(SourceAndDest[1]);
			}
			else
			{
				Input.Dest = FPaths::GetPath(Input.Source);
			}
			FPaths::NormalizeFilename(Input.Dest);
			FPakFile::MakeDirectoryFromPath(Input.Dest);

			//check for compression switches
			for (int32 Index = 0; Index < Switches.Num(); ++Index)
			{
				if (Switches[Index] == TEXT("compress"))
				{
					Input.bNeedsCompression = true;
				}
				if (Switches[Index] == TEXT("encrypt"))
				{
					Input.bNeedEncryption = true;
				}
			}
			Input.bNeedsCompression |= bCompress;
			Input.bNeedEncryption |= bEncrypt;

			UE_LOG(LogPakFile, Display, TEXT("Added file Source: %s Dest: %s"), *Input.Source, *Input.Dest);
			Entries.Add(Input);
		}			
	}
	else
	{
		// Override destination path.
		FString MountPoint;
		FParse::Value(FCommandLine::Get(), TEXT("-dest="), MountPoint);
		FPaths::NormalizeFilename(MountPoint);
		FPakFile::MakeDirectoryFromPath(MountPoint);

		// Parse comand line params. The first param after the program name is the created pak name
		for (int32 Index = 2; Index < ArgC; Index++)
		{
			// Skip switches and add everything else to the Entries array
			TCHAR* Param = ArgV[Index];
			if (Param[0] != '-')
			{
				FPakInputPair Input;
				Input.Source = Param;
				FPaths::NormalizeFilename(Input.Source);
				if (MountPoint.Len() > 0)
				{
					FString SourceDirectory( FPaths::GetPath(Input.Source) );
					FPakFile::MakeDirectoryFromPath(SourceDirectory);
					Input.Dest = Input.Source.Replace(*SourceDirectory, *MountPoint, ESearchCase::IgnoreCase);
				}
				else
				{
					Input.Dest = FPaths::GetPath(Input.Source);
					FPakFile::MakeDirectoryFromPath(Input.Dest);
				}
				FPaths::NormalizeFilename(Input.Dest);
				Entries.Add(Input);
			}
		}
	}
	UE_LOG(LogPakFile, Display, TEXT("Added %d entries to add to pak file."), Entries.Num());
}

void CollectFilesToAdd(TArray<FPakInputPair>& OutFilesToAdd, const TArray<FPakInputPair>& InEntries, const TMap<FString, uint64>& OrderMap)
{
	UE_LOG(LogPakFile, Display, TEXT("Collecting files to add to pak file..."));
	const double StartTime = FPlatformTime::Seconds();

	// Start collecting files
	TSet<FString> AddedFiles;	
	for (int32 Index = 0; Index < InEntries.Num(); Index++)
	{
		const FPakInputPair& Input = InEntries[Index];
		const FString& Source = Input.Source;
		bool bCompression = Input.bNeedsCompression;
		bool bEncryption = Input.bNeedEncryption;


		FString Filename = FPaths::GetCleanFilename(Source);
		FString Directory = FPaths::GetPath(Source);
		FPaths::MakeStandardFilename(Directory);
		FPakFile::MakeDirectoryFromPath(Directory);

		if (Filename.IsEmpty())
		{
			Filename = TEXT("*.*");
		}
		if ( Filename.Contains(TEXT("*")) )
		{
			// Add multiple files
			TArray<FString> FoundFiles;
			IFileManager::Get().FindFilesRecursive(FoundFiles, *Directory, *Filename, true, false);

			for (int32 FileIndex = 0; FileIndex < FoundFiles.Num(); FileIndex++)
			{
				FPakInputPair FileInput;
				FileInput.Source = FoundFiles[FileIndex];
				FPaths::MakeStandardFilename(FileInput.Source);
				FileInput.Dest = FileInput.Source.Replace(*Directory, *Input.Dest, ESearchCase::IgnoreCase);
				FileInput.bNeedsCompression = bCompression;
				FileInput.bNeedEncryption = bEncryption;
				if (!AddedFiles.Contains(FileInput.Source))
				{
					OutFilesToAdd.Add(FileInput);
					AddedFiles.Add(FileInput.Source);
				}
				else
				{
					int32 FoundIndex;
					OutFilesToAdd.Find(FileInput,FoundIndex);
					OutFilesToAdd[FoundIndex].bNeedEncryption |= bEncryption;
					OutFilesToAdd[FoundIndex].bNeedsCompression |= bCompression;
				}
			}
		}
		else
		{
			// Add single file
			FPakInputPair FileInput;
			FileInput.Source = Input.Source;
			FPaths::MakeStandardFilename(FileInput.Source);
			FileInput.Dest = FileInput.Source.Replace(*Directory, *Input.Dest, ESearchCase::IgnoreCase);
			FileInput.bNeedEncryption = bEncryption;
			FileInput.bNeedsCompression = bCompression;

			if (AddedFiles.Contains(FileInput.Source))
			{
				int32 FoundIndex;
				OutFilesToAdd.Find(FileInput, FoundIndex);
				OutFilesToAdd[FoundIndex].bNeedEncryption |= bEncryption;
				OutFilesToAdd[FoundIndex].bNeedsCompression |= bCompression;
			}
			else
			{
				OutFilesToAdd.AddUnique(FileInput);
			}
		}
	}

	// Sort alphabetically
	struct FInputPairSort
	{
		const TMap<FString, uint64>& OrderMap;

		FInputPairSort(const TMap<FString, uint64>& InOrderMap) : OrderMap(InOrderMap) {}
		
		FORCEINLINE bool operator()(const FPakInputPair& A, const FPakInputPair& B) const
		{
			uint64 AOrder = MAX_uint64;
			uint64 BOrder = MAX_uint64;
			const uint64* FoundOrder;
			FoundOrder = OrderMap.Find(A.Dest);
			if (FoundOrder) AOrder = *FoundOrder;
			FoundOrder = OrderMap.Find(B.Dest);
			if (FoundOrder) BOrder = *FoundOrder;
			return AOrder == BOrder ? A.Dest < B.Dest : AOrder < BOrder;
		}
	};
	OutFilesToAdd.Sort(FInputPairSort(OrderMap));
	UE_LOG(LogPakFile, Display, TEXT("Collected %d files in %.2lfs."), OutFilesToAdd.Num(), FPlatformTime::Seconds() - StartTime);
}

bool BufferedCopyFile(FArchive& Dest, FArchive& Source, const FPakEntry& Entry, void* Buffer, int64 BufferSize)
{	
	// Align down
	BufferSize = BufferSize & ~(FAES::AESBlockSize-1);
	int64 RemainingSizeToCopy = Entry.Size;
	while (RemainingSizeToCopy > 0)
	{
		const int64 SizeToCopy = FMath::Min(BufferSize, RemainingSizeToCopy);
		// If file is encrypted so we need to account for padding
		int64 SizeToRead = Entry.bEncrypted ? Align(SizeToCopy,FAES::AESBlockSize) : SizeToCopy;

		Source.Serialize(Buffer,SizeToRead);
		if (Entry.bEncrypted)
		{
			FAES::DecryptData((uint8*)Buffer,SizeToRead);
		}
		Dest.Serialize(Buffer, SizeToCopy);
		RemainingSizeToCopy -= SizeToRead;
	}
	return true;
}

bool UncompressCopyFile(FArchive& Dest, FArchive& Source, const FPakEntry& Entry, uint8*& PersistentBuffer, int64& BufferSize)
{
	if (Entry.UncompressedSize == 0)
	{
		return false;
	}

	int64 WorkingSize = Entry.CompressionBlockSize;
	int32 MaxCompressionBlockSize = FCompression::CompressMemoryBound((ECompressionFlags)Entry.CompressionMethod, WorkingSize);
	WorkingSize += MaxCompressionBlockSize;
	if (BufferSize < WorkingSize)
	{
		PersistentBuffer = (uint8*)FMemory::Realloc(PersistentBuffer, WorkingSize);
		BufferSize = WorkingSize;
	}

	uint8* UncompressedBuffer = PersistentBuffer+MaxCompressionBlockSize;

	for (uint32 BlockIndex=0, BlockIndexNum=Entry.CompressionBlocks.Num(); BlockIndex < BlockIndexNum; ++BlockIndex)
	{
		uint32 CompressedBlockSize = Entry.CompressionBlocks[BlockIndex].CompressedEnd - Entry.CompressionBlocks[BlockIndex].CompressedStart;
		uint32 UncompressedBlockSize = (uint32)FMath::Min<int64>(Entry.UncompressedSize - Entry.CompressionBlockSize*BlockIndex, Entry.CompressionBlockSize);
		Source.Seek(Entry.CompressionBlocks[BlockIndex].CompressedStart);
		uint32 SizeToRead = Entry.bEncrypted ? Align(CompressedBlockSize, FAES::AESBlockSize) : CompressedBlockSize;
		Source.Serialize(PersistentBuffer, SizeToRead);

		if (Entry.bEncrypted)
		{
			FAES::DecryptData(PersistentBuffer, SizeToRead);
		}

		if(!FCompression::UncompressMemory((ECompressionFlags)Entry.CompressionMethod,UncompressedBuffer,UncompressedBlockSize,PersistentBuffer,CompressedBlockSize))
		{
			return false;
		}
		Dest.Serialize(UncompressedBuffer,UncompressedBlockSize);
	}

	return true;
}

/**
 * Creates a pak file writer. This can be a signed writer if the encryption keys are specified in the command line
 */
FArchive* CreatePakWriter(const TCHAR* Filename)
{
	FArchive* Writer = IFileManager::Get().CreateFileWriter(Filename);
	FString KeyFilename;
	if (Writer && FParse::Value(FCommandLine::Get(), TEXT("sign="), KeyFilename, false))
	{
		FKeyPair Pair;
		if (KeyFilename.StartsWith(TEXT("0x")))
		{
			TArray<FString> KeyValueText;
			if (KeyFilename.ParseIntoArray(&KeyValueText, TEXT("+"), true) == 3)
			{
				Pair.PrivateKey.Exponent.Parse(KeyValueText[0]);
				Pair.PrivateKey.Modulus.Parse(KeyValueText[1]);
				Pair.PublicKey.Exponent.Parse(KeyValueText[2]);
				Pair.PublicKey.Modulus = Pair.PrivateKey.Modulus;
				UE_LOG(LogPakFile, Display, TEXT("Parsed signature keys from command line."));
			}
			else
			{
				UE_LOG(LogPakFile, Error, TEXT("Expected 3 values, got %d, when parsing %s"), KeyValueText.Num(), *KeyFilename);
				Pair.PrivateKey.Exponent.Zero();
			}
		}
		else if (!ReadKeysFromFile(*KeyFilename, Pair))
		{
			UE_LOG(LogPakFile, Error, TEXT("Unable to load signature keys %s."), *KeyFilename);
			Pair.PrivateKey.Exponent.Zero();
		}

		if (!Pair.PrivateKey.Exponent.IsZero())
		{
			UE_LOG(LogPakFile, Display, TEXT("Creating signed pak %s."), Filename);
			Writer = new FSignedArchiveWriter(*Writer, Pair.PublicKey, Pair.PrivateKey);
		}
		else
		{
			UE_LOG(LogPakFile, Error, TEXT("Unable to create a signed pak writer."));
			delete Writer;
			Writer = NULL;
		}		
	}
	return Writer;
}

bool CreatePakFile(const TCHAR* Filename, TArray<FPakInputPair>& FilesToAdd, const FPakCommandLineParameters& CmdLineParameters)
{	
	const double StartTime = FPlatformTime::Seconds();

	// Create Pak
	TAutoPtr<FArchive> PakFileHandle(CreatePakWriter(Filename));
	if (!PakFileHandle.IsValid())
	{
		UE_LOG(LogPakFile, Error, TEXT("Unable to create pak file \"%s\"."), Filename);
		return false;
	}

	FPakInfo Info;
	TArray<FPakEntryPair> Index;
	FString MountPoint = GetCommonRootPath(FilesToAdd);
	uint8* ReadBuffer = NULL;
	int64 BufferSize = 0;
	ECompressionFlags CompressionMethod = COMPRESS_None;
	FCompressedFileBuffer CompressedFileBuffer;

	for (int32 FileIndex = 0; FileIndex < FilesToAdd.Num(); FileIndex++)
	{
		//  Remember the offset but don't serialize it with the entry header.
		int64 NewEntryOffset = PakFileHandle->Tell();
		FPakEntryPair NewEntry;

		//check if this file requested to be compression
		int64 OriginalFileSize = IFileManager::Get().FileSize(*FilesToAdd[FileIndex].Source);
		int64 RealFileSize = OriginalFileSize + NewEntry.Info.GetSerializedSize(FPakInfo::PakFile_Version_Latest);
		CompressionMethod = (FilesToAdd[FileIndex].bNeedsCompression && OriginalFileSize > 0) ? COMPRESS_Default : COMPRESS_None;

		if (CompressionMethod != COMPRESS_None)
		{
			if (CompressedFileBuffer.CompressFileToWorkingBuffer(FilesToAdd[FileIndex], ReadBuffer, BufferSize, CompressionMethod, CmdLineParameters.CompressionBlockSize))
			{
				NewEntry.Info.CompressionMethod = CompressionMethod;
				NewEntry.Info.CompressionBlocks.AddUninitialized(CompressedFileBuffer.CompressedBlocks.Num());
				RealFileSize = CompressedFileBuffer.TotalCompressedSize + NewEntry.Info.GetSerializedSize(FPakInfo::PakFile_Version_Latest);
				NewEntry.Info.CompressionBlocks.Reset();
			}
		}

		// Account for file system block size, which is a boundary we want to avoid crossing.
		if (CmdLineParameters.FileSystemBlockSize > 0 && OriginalFileSize != INDEX_NONE && RealFileSize <= CmdLineParameters.FileSystemBlockSize)
		{
			if ((NewEntryOffset / CmdLineParameters.FileSystemBlockSize) != ((NewEntryOffset+RealFileSize) / CmdLineParameters.FileSystemBlockSize))
			{
				//File crosses a block boundary, so align it to the beginning of the next boundary
				NewEntryOffset = AlignArbitrary(NewEntryOffset, CmdLineParameters.FileSystemBlockSize);
				PakFileHandle->Seek(NewEntryOffset);
			}
		}

		bool bCopiedToPak;
		if (FilesToAdd[FileIndex].bNeedsCompression && CompressionMethod != COMPRESS_None)
		{
			bCopiedToPak = CopyCompressedFileToPak(*PakFileHandle, MountPoint, FilesToAdd[FileIndex], CompressedFileBuffer, NewEntry);
		}
		else
		{
			bCopiedToPak = CopyFileToPak(*PakFileHandle, MountPoint, FilesToAdd[FileIndex], ReadBuffer, BufferSize, NewEntry);
		}

		if (bCopiedToPak)
		{
			// Update offset now and store it in the index (and only in index)
			NewEntry.Info.Offset = NewEntryOffset;
			Index.Add(NewEntry);
			if (FilesToAdd[FileIndex].bNeedsCompression)
			{
				UE_LOG(LogPakFile, Display, TEXT("Added compressed file \"%s\", Compressed Size %lld bytes, Original Size %lld bytes."), *NewEntry.Filename, NewEntry.Info.Size, NewEntry.Info.UncompressedSize);
			}
			else
			{
				UE_LOG(LogPakFile, Display, TEXT("Added file \"%s\", %lld bytes."), *NewEntry.Filename, NewEntry.Info.Size);
			}
		}
		else
		{
			UE_LOG(LogPakFile, Warning, TEXT("Missing file \"%s\" will not be added to PAK file."), *FilesToAdd[FileIndex].Source);
		}
	}

	FMemory::Free(ReadBuffer);
	ReadBuffer = NULL;

	// Remember IndexOffset
	Info.IndexOffset = PakFileHandle->Tell();

	// Serialize Pak Index at the end of Pak File
	TArray<uint8> IndexData;
	FMemoryWriter IndexWriter(IndexData);
	IndexWriter.SetByteSwapping(PakFileHandle->ForceByteSwapping());
	int32 NumEntries = Index.Num();
	IndexWriter << MountPoint;
	IndexWriter << NumEntries;
	for (int32 EntryIndex = 0; EntryIndex < Index.Num(); EntryIndex++)
	{
		FPakEntryPair& Entry = Index[EntryIndex];
		IndexWriter << Entry.Filename;
		Entry.Info.Serialize(IndexWriter, Info.Version);
	}
	PakFileHandle->Serialize(IndexData.GetData(), IndexData.Num());

	FSHA1::HashBuffer(IndexData.GetData(), IndexData.Num(), Info.IndexHash);
	Info.IndexSize = IndexData.Num();

	// Save trailer (offset, size, hash value)
	Info.Serialize(*PakFileHandle);

	UE_LOG(LogPakFile, Display, TEXT("Added %d files, %lld bytes total, time %.2lfs."), Index.Num(), PakFileHandle->TotalSize(), FPlatformTime::Seconds() - StartTime);

	PakFileHandle->Close();
	PakFileHandle.Reset();

	return true;
}

bool TestPakFile(const TCHAR* Filename)
{	
	FPakFile PakFile(Filename, FParse::Param(FCommandLine::Get(), TEXT("signed")));
	if (PakFile.IsValid())
	{
		UE_LOG(LogPakFile, Display, TEXT("Checking pak file \"%s\". This may take a while..."), Filename);
		FArchive& PakReader = *PakFile.GetSharedReader(NULL);
		int32 ErrorCount = 0;
		int32 FileCount = 0;

		for (FPakFile::FFileIterator It(PakFile); It; ++It, ++FileCount)
		{
			const FPakEntry& Entry = It.Info();
			void* FileContents = FMemory::Malloc(Entry.Size);
			PakReader.Seek(Entry.Offset);
			uint32 SerializedCrcTest = 0;
			FPakEntry EntryInfo;
			EntryInfo.Serialize(PakReader, PakFile.GetInfo().Version);
			if (EntryInfo != Entry)
			{
				UE_LOG(LogPakFile, Error, TEXT("Serialized hash mismatch for \"%s\"."), *It.Filename());
				ErrorCount++;
			}
			PakReader.Serialize(FileContents, Entry.Size);
		
			uint8 TestHash[20];
			FSHA1::HashBuffer(FileContents, Entry.Size, TestHash);
			if (FMemory::Memcmp(TestHash, Entry.Hash, sizeof(TestHash)) != 0)
			{
				UE_LOG(LogPakFile, Error, TEXT("Hash mismatch for \"%s\"."), *It.Filename());
				ErrorCount++;
			}
			else
			{
				UE_LOG(LogPakFile, Display, TEXT("\"%s\" OK."), *It.Filename());
			}
			FMemory::Free(FileContents);
		}
		if (ErrorCount == 0)
		{
			UE_LOG(LogPakFile, Display, TEXT("Pak file \"%s\" healthy, %d files checked."), Filename, FileCount);
		}
		else
		{
			UE_LOG(LogPakFile, Display, TEXT("Pak file \"%s\" corrupted (%d errors ouf of %d files checked.)."), Filename, ErrorCount, FileCount);
		}
		return (ErrorCount == 0);
	}
	else
	{
		UE_LOG(LogPakFile, Error, TEXT("Unable to open pak file \"%s\"."), Filename);
		return false;
	}
}

bool ExtractFilesFromPak(const TCHAR* InPakFilename, const TCHAR* InDestPath)
{
	FPakFile PakFile(InPakFilename, FParse::Param(FCommandLine::Get(), TEXT("signed")));
	if (PakFile.IsValid())
	{
		FString DestPath(InDestPath);
		FArchive& PakReader = *PakFile.GetSharedReader(NULL);
		const int64 BufferSize = 8 * 1024 * 1024; // 8MB buffer for extracting
		void* Buffer = FMemory::Malloc(BufferSize);
		int64 CompressionBufferSize = 0;
		uint8* PersistantCompressionBuffer = NULL;
		int32 ErrorCount = 0;
		int32 FileCount = 0;

		for (FPakFile::FFileIterator It(PakFile); It; ++It, ++FileCount)
		{
			const FPakEntry& Entry = It.Info();
			PakReader.Seek(Entry.Offset);
			uint32 SerializedCrcTest = 0;
			FPakEntry EntryInfo;
			EntryInfo.Serialize(PakReader, PakFile.GetInfo().Version);
			if (EntryInfo == Entry)
			{
				FString DestFilename(DestPath / It.Filename());
				TAutoPtr<FArchive> FileHandle(IFileManager::Get().CreateFileWriter(*DestFilename));
				if (FileHandle.IsValid())
				{
					if (Entry.CompressionMethod == COMPRESS_None)
					{
						BufferedCopyFile(*FileHandle, PakReader, Entry, Buffer, BufferSize);
					}
					else
					{
						UncompressCopyFile(*FileHandle, PakReader, Entry, PersistantCompressionBuffer, CompressionBufferSize);
					}
					UE_LOG(LogPakFile, Display, TEXT("Extracted \"%s\" to \"%s\"."), *It.Filename(), *DestFilename);
				}
				else
				{
					UE_LOG(LogPakFile, Error, TEXT("Unable to create file \"%s\"."), *DestFilename);
					ErrorCount++;
				}
			}
			else
			{
				UE_LOG(LogPakFile, Error, TEXT("Serialized hash mismatch for \"%s\"."), *It.Filename());
				ErrorCount++;
			}
		}
		FMemory::Free(Buffer);
		FMemory::Free(PersistantCompressionBuffer);

		UE_LOG(LogPakFile, Error, TEXT("Finished extracting %d files (including %d errors)."), FileCount, ErrorCount);

		return true;
	}
	else
	{
		UE_LOG(LogPakFile, Error, TEXT("Unable to open pak file \"%s\"."), InPakFilename);
		return false;
	}
}

/**
 * Application entry point
 * Params:
 *   -Test test if the pak file is healthy
 *   -Extract extracts pak file contents (followed by a path, i.e.: -extract D:\ExtractedPak)
 *   -Create=filename response file to create a pak file with
 *   -Sign=filename use the key pair in filename to sign a pak file, or: -sign=key_hex_values_separated_with_+, i.e: -sign=0x123456789abcdef+0x1234567+0x12345abc
 *    where the first number is the private key exponend, the second one is modulus and the third one is the public key exponent.
 *   -Signed use with -extract and -test to let the code know this is a signed pak
 *   -GenerateKeys=filename generates encryption key pair for signing a pak file
 *   -P=prime will use a predefined prime number for generating encryption key file
 *   -Q=prime same as above, P != Q, GCD(P, Q) = 1 (which is always true if they're both prime)
 *   -GeneratePrimeTable=filename generates a prime table for faster prime number generation (.inl file)
 *   -TableMax=number maximum prime number in the generated table (default is 10000)
 *
 * @param	ArgC	Command-line argument count
 * @param	ArgV	Argument strings
 */
INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	// start up the main loop
	GEngineLoop.PreInit(ArgC, ArgV);
	
	if (ArgC < 2)
	{
		UE_LOG(LogPakFile, Error, TEXT("No pak file name specified."));
		return 1;
	}

	FPakCommandLineParameters CmdLineParameters;
	int32 Result = 0;
	FString KeyFilename;
	if (FParse::Value(FCommandLine::Get(), TEXT("GenerateKeys="), KeyFilename, false))
	{
		Result = GenerateKeys(*KeyFilename) ? 0 : 1;
	}
	else if (FParse::Value(FCommandLine::Get(), TEXT("GeneratePrimeTable="), KeyFilename, false))
	{
		int64 MaxPrimeValue = 10000;
		FParse::Value(FCommandLine::Get(), TEXT("TableMax="), MaxPrimeValue);
		GeneratePrimeNumberTable(MaxPrimeValue, *KeyFilename);
	}
	else 
	{
		FString PakFilename(ArgV[1]);
		FPaths::MakeStandardFilename(PakFilename);

		if (FParse::Param(FCommandLine::Get(), TEXT("Test")))
		{
			Result = TestPakFile(*PakFilename) ? 0 : 1;
		}
		else if (FParse::Param(FCommandLine::Get(), TEXT("Extract")))
		{
			if (ArgC < 4)
			{
				UE_LOG(LogPakFile, Error, TEXT("No extraction path specified."));
				Result = 1;
			}
			else
			{
				FString DestPath = (ArgV[2][0] == '-') ? ArgV[3] : ArgV[2];
				Result = ExtractFilesFromPak(*PakFilename, *DestPath) ? 0 : 1;
			}
		}
		else
		{
			// List of all items to add to pak file
			TArray<FPakInputPair> Entries;
			ProcessCommandLine(ArgC, ArgV, Entries, CmdLineParameters);
			TMap<FString, uint64> OrderMap;
			ProcessOrderFile(ArgC, ArgV, OrderMap);

			if (Entries.Num() == 0)
			{
				UE_LOG(LogPakFile, Error, TEXT("No files specified to add to pak file."));
				Result = 1;
			}
			else
			{
				// Start collecting files
				TArray<FPakInputPair> FilesToAdd;
				CollectFilesToAdd(FilesToAdd, Entries, OrderMap);

				Result = CreatePakFile(*PakFilename, FilesToAdd, CmdLineParameters) ? 0 : 1;
			}
		}
	}

	return Result;
}
