#include "SummitSetupCommandlet.h"

#include "SummitGameMode.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogSummitSetup, Log, All);

namespace
{
	// Landscape layout used by Tools/terrain: 63-quad sections, 2x2 sections per component.
	constexpr int32 QuadsPerSection = 63;
	constexpr int32 SectionsPerComponent = 2;

	FVector ReadVector(const TSharedPtr<FJsonObject>& Obj)
	{
		return FVector(Obj->GetNumberField(TEXT("x")), Obj->GetNumberField(TEXT("y")), Obj->GetNumberField(TEXT("z")));
	}

	template <typename T>
	T* AddNode(UMaterial* Mat, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Mat, T::StaticClass(), X, Y));
	}

	void Link(UMaterialExpression* From, UMaterialExpression* To, const TCHAR* Input)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(From, TEXT(""), To, Input);
	}

	UMaterialExpression* MaskZ(UMaterial* Mat, UMaterialExpression* Source, int32 Y)
	{
		UMaterialExpressionComponentMask* Mask = AddNode<UMaterialExpressionComponentMask>(Mat, -1200, Y);
		Mask->R = Mask->G = Mask->A = 0;
		Mask->B = 1;
		Link(Source, Mask, TEXT(""));
		return Mask;
	}

	// Saturate((Input - Offset) * Gain): a linear ramp from 0 at Offset to 1 at Offset + 1/Gain.
	UMaterialExpression* Ramp(UMaterial* Mat, UMaterialExpression* Input, float Offset, float Gain, int32 Y)
	{
		UMaterialExpressionSubtract* Sub = AddNode<UMaterialExpressionSubtract>(Mat, -900, Y);
		Sub->ConstB = Offset;
		Link(Input, Sub, TEXT("A"));
		UMaterialExpressionMultiply* Mul = AddNode<UMaterialExpressionMultiply>(Mat, -700, Y);
		Mul->ConstB = Gain;
		Link(Sub, Mul, TEXT("A"));
		UMaterialExpressionSaturate* Sat = AddNode<UMaterialExpressionSaturate>(Mat, -500, Y);
		Link(Mul, Sat, TEXT(""));
		return Sat;
	}

	UMaterialExpression* Color(UMaterial* Mat, const FLinearColor& C, int32 Y)
	{
		UMaterialExpressionConstant3Vector* Node = AddNode<UMaterialExpressionConstant3Vector>(Mat, -500, Y);
		Node->Constant = C;
		return Node;
	}

	// Placeholder landscape look until Megascans layers exist: snow on gentler slopes above ~5,000 m, dark rock on
	// steep faces, brown moraine below. Altitude comes from world Z, so it follows the real snowline.
	UMaterial* BuildLandscapeMaterial(const FString& PackageName, double SeaLevelOffsetM)
	{
		UPackage* Pkg = CreatePackage(*PackageName);
		UMaterial* Mat = NewObject<UMaterial>(Pkg, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);

		UMaterialExpression* Pos = AddNode<UMaterialExpressionWorldPosition>(Mat, -1500, 0);
		UMaterialExpressionMultiply* ToMetres = AddNode<UMaterialExpressionMultiply>(Mat, -1000, 0);
		ToMetres->ConstB = 0.01f;
		Link(MaskZ(Mat, Pos, 0), ToMetres, TEXT("A"));
		UMaterialExpressionAdd* Altitude = AddNode<UMaterialExpressionAdd>(Mat, -1000, 100);
		Altitude->ConstB = static_cast<float>(SeaLevelOffsetM);
		Link(ToMetres, Altitude, TEXT("A"));
		UMaterialExpression* High = Ramp(Mat, Altitude, 4900.f, 1.f / 600.f, 0);

		UMaterialExpression* NormalZ = MaskZ(Mat, AddNode<UMaterialExpressionVertexNormalWS>(Mat, -1500, 400), 400);
		UMaterialExpression* Flat = Ramp(Mat, NormalZ, 0.70f, 6.f, 400);     // snow holds below ~45 degrees
		UMaterialExpression* Steep = Ramp(Mat, NormalZ, 0.80f, -8.f, 700);   // 1 on steep faces

		UMaterialExpressionMultiply* Snow = AddNode<UMaterialExpressionMultiply>(Mat, -300, 200);
		Link(Flat, Snow, TEXT("A"));
		Link(High, Snow, TEXT("B"));

		UMaterialExpressionLinearInterpolate* Ground = AddNode<UMaterialExpressionLinearInterpolate>(Mat, -300, 600);
		Link(Color(Mat, FLinearColor(0.16f, 0.13f, 0.10f), 900), Ground, TEXT("A"));
		Link(Color(Mat, FLinearColor(0.08f, 0.075f, 0.07f), 1000), Ground, TEXT("B"));
		Link(Steep, Ground, TEXT("Alpha"));

		UMaterialExpressionLinearInterpolate* Base = AddNode<UMaterialExpressionLinearInterpolate>(Mat, -100, 300);
		Link(Ground, Base, TEXT("A"));
		Link(Color(Mat, FLinearColor(0.82f, 0.86f, 0.92f), 1100), Base, TEXT("B"));
		Link(Snow, Base, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);

		UMaterialExpressionLinearInterpolate* Rough = AddNode<UMaterialExpressionLinearInterpolate>(Mat, -100, 500);
		Rough->ConstA = 0.85f;
		Rough->ConstB = 0.45f;
		Link(Snow, Rough, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);

		UMaterialEditingLibrary::RecompileMaterial(Mat);
		return Mat;
	}

	bool SaveAsset(UObject* Asset)
	{
		UPackage* Pkg = Asset->GetPackage();
		Pkg->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Pkg, Asset, *File, Args);
	}
}

USummitSetupCommandlet::USummitSetupCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 USummitSetupCommandlet::Main(const FString& Params)
{
	FString TerrainDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/terrain/output/everest"));
	FString MapName = TEXT("/Game/Maps/L_Everest");
	FParse::Value(*Params, TEXT("Terrain="), TerrainDir);
	FParse::Value(*Params, TEXT("Map="), MapName);

	// ---- Terrain metadata (written by Tools/terrain/summit_terrain.py build) ----
	TArray<FString> JsonFiles;
	IFileManager::Get().FindFiles(JsonFiles, *FPaths::Combine(TerrainDir, TEXT("*.json")), true, false);
	if (JsonFiles.Num() != 1)
	{
		UE_LOG(LogSummitSetup, Error, TEXT("Expected exactly one metadata .json in %s, found %d. Run Tools/terrain first."), *TerrainDir, JsonFiles.Num());
		return 1;
	}
	FString JsonText;
	TSharedPtr<FJsonObject> Meta;
	if (!FFileHelper::LoadFileToString(JsonText, *FPaths::Combine(TerrainDir, JsonFiles[0]))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Meta) || !Meta.IsValid())
	{
		UE_LOG(LogSummitSetup, Error, TEXT("Could not parse %s"), *JsonFiles[0]);
		return 1;
	}
	const TSharedPtr<FJsonObject> Import = Meta->GetObjectField(TEXT("ue_import"));
	const FVector Location = ReadVector(Import->GetObjectField(TEXT("location_cm")));
	const FVector Scale = ReadVector(Import->GetObjectField(TEXT("scale")));
	const double SeaLevelWorldZCm = Import->GetNumberField(TEXT("sea_level_world_z_cm"));
	const double SeaLevelOffsetM = Import->GetNumberField(TEXT("sea_level_offset_m"));
	const int32 Size = Meta->GetObjectField(TEXT("grid"))->GetIntegerField(TEXT("size_px"));

	const int32 QuadsPerComponent = QuadsPerSection * SectionsPerComponent;
	if ((Size - 1) % QuadsPerComponent != 0)
	{
		UE_LOG(LogSummitSetup, Error, TEXT("Heightmap size %d is not N*%d+1; rebuild with --ue-size 1009/2017/4033/8129."), Size, QuadsPerComponent);
		return 1;
	}

	// Spawn at Everest Base Camp if the terrain covers it; terrain x is east, y north, and UE +Y points south.
	FVector Spawn(0.0, 0.0, Location.Z);
	for (const TSharedPtr<FJsonValue>& Value : Meta->GetArrayField(TEXT("landmarks")))
	{
		const TSharedPtr<FJsonObject> Mark = Value->AsObject();
		if (Mark->GetStringField(TEXT("name")).Contains(TEXT("Base Camp")) && Mark->GetBoolField(TEXT("inside")))
		{
			Spawn = FVector(Mark->GetNumberField(TEXT("x_m")) * 100.0, -Mark->GetNumberField(TEXT("y_m")) * 100.0,
				Mark->GetNumberField(TEXT("ue_world_z_cm")) + 250.0);
		}
	}

	// ---- Heights (.r16: little-endian uint16, row 0 = north) ----
	TArray<uint8> Raw;
	const FString R16 = FPaths::Combine(TerrainDir, FPaths::GetBaseFilename(JsonFiles[0]) + TEXT(".r16"));
	if (!FFileHelper::LoadFileToArray(Raw, *R16) || Raw.Num() != Size * Size * 2)
	{
		UE_LOG(LogSummitSetup, Error, TEXT("Could not read %dx%d heights from %s"), Size, Size, *R16);
		return 1;
	}
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(Size * Size);
	FMemory::Memcpy(Heights.GetData(), Raw.GetData(), Raw.Num());

	// ---- Material: keep the textured one from Tools/setup/build_landscape_material.py if it exists ----
	const FString MaterialPath = TEXT("/Game/Summit/Materials/M_SummitLandscape");
	UMaterial* LandscapeMaterial = LoadObject<UMaterial>(nullptr, *(MaterialPath + TEXT(".M_SummitLandscape")), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!LandscapeMaterial)
	{
		LandscapeMaterial = BuildLandscapeMaterial(MaterialPath, SeaLevelOffsetM);
		if (!SaveAsset(LandscapeMaterial))
		{
			UE_LOG(LogSummitSetup, Error, TEXT("Failed to save the landscape material"));
			return 1;
		}
	}

	// ---- Map ----
	UPackage* MapPackage = CreatePackage(*MapName);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, true, FName(*FPackageName::GetShortName(MapName)), MapPackage);
	World->SetFlags(RF_Public | RF_Standalone);
	World->GetWorldSettings()->DefaultGameMode = ASummitGameMode::StaticClass();

	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location, FRotator::ZeroRotator);
	Landscape->SetActorRelativeScale3D(Scale);
	Landscape->LandscapeMaterial = LandscapeMaterial;
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((Size * Size) / (2048 * 2048) + 1), (uint32)2);
	TMap<FGuid, TArray<uint16>> HeightsPerLayer;
	HeightsPerLayer.Add(FGuid(), MoveTemp(Heights));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayers;
	MaterialLayers.Add(FGuid(), {});
	Landscape->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, SectionsPerComponent, QuadsPerSection, HeightsPerLayer,
		*R16, MaterialLayers, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
	{
		Info->UpdateLayerInfoMap(Landscape);
	}
	Landscape->SetActorLabel(TEXT("Everest"));

	// Sun: movable, tagged for USummitWeatherSubsystem, which sets its angle from the clock and latitude.
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 300000.0), FRotator(-35.0, 120.0, 0.0));
	UDirectionalLightComponent* SunLight = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
	Sun->SetMobility(EComponentMobility::Movable);
	SunLight->SetAtmosphereSunLight(true);
	Sun->Tags.Add(TEXT("Sun"));
	Sun->SetActorLabel(TEXT("Sun"));

	// Planet surface at real sea level, so the sky thins and darkens with altitude as it should.
	ASkyAtmosphere* Sky = World->SpawnActor<ASkyAtmosphere>(FVector(0.0, 0.0, SeaLevelWorldZCm), FRotator::ZeroRotator);
	Sky->GetComponent()->TransformMode = ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform;

	// AVolumetricCloud is not exported from Engine, so spawn it by class path.
	if (UClass* CloudClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.VolumetricCloud")))
	{
		AActor* Clouds = World->SpawnActor<AActor>(CloudClass, FVector::ZeroVector, FRotator::ZeroRotator);
		UVolumetricCloudComponent* Layer = Clouds->FindComponentByClass<UVolumetricCloudComponent>();
		Layer->SetLayerBottomAltitude(6.5f);  // km above sea level: cloud sea below the summit
		Layer->SetLayerHeight(4.f);
	}

	ASkyLight* SkyLight = World->SpawnActor<ASkyLight>(FVector(0.0, 0.0, 200000.0), FRotator::ZeroRotator);
	SkyLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	SkyLight->GetLightComponent()->bRealTimeCapture = true;

	AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(FVector(0.0, 0.0, Spawn.Z), FRotator::ZeroRotator);
	Fog->GetComponent()->SetFogDensity(0.004f);
	Fog->GetComponent()->SetVolumetricFog(true);

	World->SpawnActor<APlayerStart>(Spawn, FRotator::ZeroRotator);

	const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, MapName);
	UE_LOG(LogSummitSetup, Display, TEXT("%s %s: %dx%d landscape, scale %s, spawn %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"),
		*MapName, Size, Size, *Scale.ToString(), *Spawn.ToString());

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return bSaved ? 0 : 1;
}
