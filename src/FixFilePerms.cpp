#include "pch.h"
#include "FixFilePerms.h"

//	WARNING: This code is shamelessly stolen from Wunkolo's UWPDdumper project :|
//	https://github.com/Wunkolo/UWPDumper
//	go to code :P 
//	https://github.com/Wunkolo/UWPDumper/blob/9fb0a040e674521c1413276bcea6e4e708f34d19/UWPInjector/source/main.cpp#L226 17.10.2020


// Setting DLL access controls
#include <stdio.h>
#include <string>
#include <Aclapi.h>
#include <Sddl.h>

DWORD SetAccessControl(std::wstring &ExecutableName, const wchar_t* AccessString, const wchar_t** failureStage)
{
	auto stage = [failureStage](const wchar_t* name) { if (failureStage) *failureStage=name; };
	stage(L"GetNamedSecurityInfoW / read DLL permissions");
	DWORD result = ERROR_SUCCESS;
	PSECURITY_DESCRIPTOR SecurityDescriptor = nullptr;
	EXPLICIT_ACCESSW ExplicitAccess = { 0 };

	ACL* AccessControlCurrent = nullptr;
	ACL* AccessControlNew = nullptr;

	SECURITY_INFORMATION SecurityInfo = DACL_SECURITY_INFORMATION;
	PSID SecurityIdentifier = nullptr;

	if (
		(result = GetNamedSecurityInfoW(
			ExecutableName.c_str(),
			SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION,
			nullptr,
			nullptr,
			&AccessControlCurrent,
			nullptr,
			&SecurityDescriptor
		)) == ERROR_SUCCESS
		)
	{
		stage(L"ConvertStringSidToSidW / application package identity");
		if (!ConvertStringSidToSidW(AccessString, &SecurityIdentifier)) result = GetLastError();
		if (SecurityIdentifier != nullptr)
		{
			ExplicitAccess.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
			ExplicitAccess.grfAccessMode = SET_ACCESS;
			ExplicitAccess.grfInheritance = NO_INHERITANCE;
			ExplicitAccess.Trustee.TrusteeForm = TRUSTEE_IS_SID;
			ExplicitAccess.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
			ExplicitAccess.Trustee.ptstrName = reinterpret_cast<wchar_t*>(SecurityIdentifier);

			stage(L"SetEntriesInAclW / prepare DLL permissions");
			if (
				(result = SetEntriesInAclW(
					1,
					&ExplicitAccess,
					AccessControlCurrent,
					&AccessControlNew
				)) == ERROR_SUCCESS
				)
			{
				stage(L"SetNamedSecurityInfoW / apply DLL permissions");
				result = SetNamedSecurityInfoW(
					const_cast<wchar_t*>(ExecutableName.c_str()),
					SE_FILE_OBJECT,
					SecurityInfo,
					nullptr,
					nullptr,
					AccessControlNew,
					nullptr
				);
			}
		}
	}
	if (SecurityDescriptor)
	{
		LocalFree(reinterpret_cast<HLOCAL>(SecurityDescriptor));
	}
	if (AccessControlNew)
	{
		LocalFree(reinterpret_cast<HLOCAL>(AccessControlNew));
	}
	if (SecurityIdentifier) LocalFree(SecurityIdentifier);
	return result;
}
