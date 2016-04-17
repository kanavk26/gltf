//
//  GLTF.cpp
//  GLTFWork
//
//  Created by Ryan Bartley on 6/19/14.
//
//

#include "GLTF.h"

#include "cinder/gl/Vbo.h"
#include "cinder/Utilities.h"
#include "cinder/Log.h"
#include "cinder/DataSource.h"
#include "cinder/Base64.h"
#include "cinder/Log.h"

using namespace ci;
using namespace ci::gl;
using namespace std;

namespace gltf {
	
struct BinaryHeader {
	std::array<uint8_t, 4>	magic;
	uint32_t				version;
	uint32_t				length;
	uint32_t				sceneLength;
	uint32_t				sceneFormat;
};
	
FileRef File::create( const ci::DataSourceRef &gltfFile )
{
	return FileRef( new File( gltfFile ) );
}
	
File::File( const ci::DataSourceRef &gltfFile )
: mGltfPath( gltfFile->getFilePath().parent_path() )
{
	std::string gltfJson;
	verifyFile( gltfFile, gltfJson );
	
	Json::Features features;
	features.allowComments_ = true;
	features.strictRoot_ = true;
	
	Json::Reader reader( features );
	try {
		reader.parse( gltfJson, mGltfTree );
	}
	catch ( const std::runtime_error &e ) {
		CI_LOG_E( "Error parsing gltf file " << e.what() );
	}
//	cout << mGltfTree.toStyledString() << endl;
	loadExtensions();
	if( ! mGltfTree["asset"].isNull() )
		setAssetInfo( mGltfTree["asset"] );
	load();
}
	
void File::verifyFile( const ci::DataSourceRef &data, std::string &gltfJson )
{
	auto pathExtension = data->getFilePath().extension().string();
	auto binary = pathExtension == ".glb";
	if( binary ) {
		auto buffer = data->getBuffer();
		auto header = reinterpret_cast<BinaryHeader*>( buffer->getData() );
		
		auto sceneStart = reinterpret_cast<uint8_t*>( header + 1 );
		gltfJson.append( sceneStart, sceneStart + header->sceneLength );
		
		auto binaryStart = sceneStart + header->sceneLength;
		auto binarySize = header->length - header->sceneLength - sizeof( BinaryHeader );
		
		mBuffer = ci::Buffer::create( binarySize );
		memcpy( mBuffer->getData(), binaryStart, binarySize );
	}
	else
		gltfJson = loadString( data );
}
	
void File::load()
{
	auto gltfTypes = mGltfTree.getMemberNames();
	auto hasMaterialsCommon = hasExtension( "KHR_materials_common" );
	for( auto &typeName : gltfTypes ) {
		auto &typeObj = mGltfTree[typeName];
		if( typeName == "scene" ) {
			mDefaultScene = typeObj.asString();
			continue;
		}
		else if( typeName == "extensionsUsed" )
			continue;
		else if( typeName == "extensions" ) {
			if( hasMaterialsCommon ) {
				auto &lights = typeObj["KHR_materials_common"]["lights"];
				auto lightKeys = lights.getMemberNames();
				for ( auto &lightKey : lightKeys ) {
					addLightInfo( lightKey, lights[lightKey] );
				}
			}
		}
		auto typeKeys = typeObj.getMemberNames();
		for( auto &typeKey : typeKeys ) {
			auto &obj = typeObj[typeKey];
			if( typeName == "accessors" )
				addAccessorInfo( typeKey, obj );
			else if( typeName == "animations" )
				addAnimationInfo( typeKey, obj );
			else if( typeName == "bufferViews" )
				addBufferViewInfo( typeKey, obj );
			else if( typeName == "buffers" )
				addBufferInfo( typeKey, obj );
			else if( typeName == "cameras" )
				addCameraInfo( typeKey, obj );
			else if( typeName == "images" )
				addImageInfo( typeKey, obj );
			else if( typeName == "materials" )
				addMaterialInfo( typeKey, obj );
			else if( typeName == "meshes" )
				addMeshInfo( typeKey, obj );
			else if( typeName == "nodes" )
				addNodeInfo( typeKey, obj );
			else if( typeName == "programs" )
				addProgramInfo( typeKey, obj );
			else if( typeName == "samplers" )
				addSamplerInfo( typeKey, obj );
			else if( typeName == "scenes" )
				addSceneInfo( typeKey, obj );
			else if( typeName == "shaders" )
				addShaderInfo( typeKey, obj );
			else if( typeName == "skins" )
				addSkinInfo( typeKey, obj );
			else if( typeName == "techniques" )
				addTechniqueInfo( typeKey, obj );
			else if( typeName == "textures" )
				addTextureInfo( typeKey, obj );
		}
	}
	// setup heirarchy for traversal.
	for( auto &scenes : mGltfTree["scenes"] ) {
		for( auto & node : scenes["nodes"] ) {
			setParentForChildren( nullptr, node.asString() );
		}
	}
}
	
void File::setParentForChildren( Node *parent, const std::string &childKey )
{
	auto foundChild = mNodes.find( childKey );
	auto &currentNode = foundChild->second;
	currentNode.parent = parent;
	if( parent )
		parent->children.push_back( &currentNode );
	auto children = mGltfTree["nodes"][childKey]["children"];
	if( ! children.empty() ) {
		for( auto & child : children ) {
			setParentForChildren( &currentNode, child.asString() );
		}
	}
}
	
void File::loadExtensions()
{
	if( ! mGltfTree["extensionsUsed"].isNull() && mGltfTree["extensionsUsed"].isArray() ) {
		auto &extensions = mGltfTree["extensionsUsed"];
		std::transform( begin( extensions ), end( extensions ), std::back_inserter( mExtensions ),
		[]( const Json::Value &val ){ return val.asString(); } );
		std::sort( begin( mExtensions ), end( mExtensions ) );
	}
}

bool File::hasExtension( const std::string &extension ) const
{
	return std::binary_search( begin( mExtensions ), end( mExtensions ), extension );
}
	
const Scene& File::getDefaultScene() const
{
	if( ! mDefaultScene.empty() ) {
		auto found = mScenes.find( mDefaultScene );
		return found->second;
	}
	else {
		auto found = mScenes.begin();
		return found->second;
	}
}

const Accessor& File::getAccessorInfo( const std::string& key ) const
{
	auto found = mAccessors.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Accessor accessor )
{
	mAccessors[key] = move(accessor);
}

void File::addAccessorInfo( const std::string &key, const Json::Value &accessorInfo )
{
	// Required points.
	CI_ASSERT( accessorInfo["bufferView"].isString() );
	CI_ASSERT( accessorInfo["byteOffset"].isNumeric() );
	CI_ASSERT( accessorInfo["componentType"].isNumeric() );
	CI_ASSERT( accessorInfo["type"].isString() );
	CI_ASSERT( accessorInfo["count"].isNumeric() );

	Accessor ret;
	auto bufferViewKey = accessorInfo["bufferView"].asString();
	auto &bufferView = mBufferViews[bufferViewKey];
	ret.bufferView = &bufferView;
	ret.byteOffset = accessorInfo["byteOffset"].asUInt();
	ret.count = accessorInfo["count"].asUInt();
	
	auto type = accessorInfo["type"].asString();
	if( type == "SCALAR" )	  ret.dataType = Accessor::DataType::SCALAR;
	else if( type == "VEC2" ) ret.dataType = Accessor::DataType::VEC2;
	else if( type == "VEC3" ) ret.dataType = Accessor::DataType::VEC3;
	else if( type == "VEC4" ) ret.dataType = Accessor::DataType::VEC4;
	else if( type == "MAT2" ) ret.dataType = Accessor::DataType::MAT2;
	else if( type == "MAT3" ) ret.dataType = Accessor::DataType::MAT3;
	else if( type == "MAT4" ) ret.dataType = Accessor::DataType::MAT4;
	else					  CI_ASSERT_MSG( false, "Unknown data type" );
	
	ret.componentType = static_cast<Accessor::ComponentType>( accessorInfo["componentType"].asUInt() );
	ret.name = accessorInfo["name"].asString();
	ret.extras = accessorInfo["extras"];

	if( !accessorInfo["byteStride"].isNull() )
		ret.byteStride = accessorInfo["byteStride"].asUInt();

	auto &maxElem = accessorInfo["max"];
	auto &minElem = accessorInfo["min"];
	if( !maxElem.isNull() && !minElem.isNull() ) {
		auto maxSize = maxElem.size();
		auto minSize = minElem.size();
		CI_ASSERT( maxSize == minSize );
		ret.max.resize( maxSize );
		ret.min.resize( minSize );
		for( int i = 0; i < maxSize; i++ ) {
			ret.max[i] = maxElem[i].asFloat();
			ret.min[i] = minElem[i].asFloat();
		}
	}
	add( key, move(ret) );
}
	
const Animation& File::getAnimationInfo( const std::string &key ) const
{
	auto found = mAnimations.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Animation animation )
{
	mAnimations[key] = move( animation );
}

void File::addAnimationInfo( const std::string &key, const Json::Value &animationInfo )
{
	Animation ret;
	auto &channels = animationInfo["channels"];
	for( auto &channel : channels ) {
		auto &target = channel["target"];

		CI_ASSERT( channel["sampler"].isString() );
		CI_ASSERT( channel["target"].isObject() );
		CI_ASSERT( target["id"].isString() );
		CI_ASSERT( target["path"].isString() );

		Animation::Channel animChannel;
		auto targetNodeKey = target["id"].asString();
		auto &targetNode = mNodes[targetNodeKey];
		animChannel.targetId = targetNodeKey;
		animChannel.target = &targetNode;
		animChannel.sampler = channel["sampler"].asString();
		animChannel.path = target["path"].asString();
		animChannel.channelExtras = channel["extras"];
		animChannel.targetExtras = channel["extras"];
		ret.channels.emplace_back( move( animChannel ) );
	}
	
	// This isn't good but I don't know any quicker way.
	ret.target = ret.channels[0].targetId;

	auto &samplers = animationInfo["samplers"];
	for( auto &sampler : samplers ) {
		CI_ASSERT( sampler["input"].isString() );
		CI_ASSERT( sampler["output"].isString() );
		
		Animation::Sampler animSampler;
		animSampler.input = sampler["input"].asString();
		animSampler.output = sampler["output"].asString();
		if( sampler["interpolation"].isString() )
			if( sampler["interpolation"].asString() == "LINEAR" )
				animSampler.type = Animation::Sampler::LerpType::LINEAR;
		
		ret.samplers.emplace_back( move( animSampler ) );
	}

	ret.name = animationInfo["name"].asString();
	auto &params = animationInfo["parameters"];
	auto paramKeys = params.getMemberNames();
	for( auto & key : paramKeys ) {
		if( key == "TIME" ) {
			auto accessorKey = params[key].asString();
			auto &accessor = mAccessors[accessorKey];
			ret.timeAccessor = &accessor;
		}
		else {
			auto accessorKey = params[key].asString();
			auto &accessor = mAccessors[accessorKey];
			Animation::Parameter param;
			param.accessor = &accessor;
			param.parameter = key;
			ret.parameters.emplace_back( move( param ) );
		}
	}
	ret.extras = animationInfo["extras"];

	add( key, move( ret ) );
}
	
const Asset& File::getAssetInfo() const
{
	return mAssetInfo;
}
	
void File::setAssetInfo( const Json::Value &assetInfo )
{
	CI_ASSERT( ! assetInfo["version"].isNull() );
	mAssetInfo.version = assetInfo["version"].asString();
	
	if( assetInfo["profile"].isObject() ) {
		if( assetInfo["profile"]["api"].isString() )
			mAssetInfo.profile.api = assetInfo["profile"]["api"].asString();
		if( assetInfo["profile"]["version"].isString() )
			mAssetInfo.profile.version = assetInfo["profile"]["version"].asString();
	}
	
	mAssetInfo.copyright = assetInfo["copyright"].asString();
	mAssetInfo.generator = assetInfo["generator"].asString();
	mAssetInfo.premultipliedAlpha = assetInfo["premultipliedAlpha"].asBool();
}
	
const gltf::Buffer& File::getBufferInfo( const std::string &name ) const
{
	auto found = mBuffers.find( name );
	return found->second;
}
	
template<>
void File::add( const std::string &key, gltf::Buffer buffer )
{
	mBuffers[key] = move( buffer );
}
	
void File::addBufferInfo( const std::string &key, const Json::Value &bufferInfo )
{
	CI_ASSERT( bufferInfo["uri"].isString() );
	
	gltf::Buffer ret;
	auto uri = bufferInfo["uri"].asString();
	
	auto pos = uri.find_first_of(',');
	if( pos != std::string::npos ) {
		ret.uri = uri.substr( 0, pos );
		auto data = uri.substr( pos + 1, uri.size() );
		auto buffer = fromBase64( data );
		ret.data = BufferRef( new ci::Buffer( std::move( buffer ) ) );
	}
	else {
		if( key == "binary_glTF" )
			ret.data = mBuffer;
		else
			ret.data = loadFile( mGltfPath / uri )->getBuffer();
	}
	
	ret.type = bufferInfo["type"].asString();
	ret.byteLength = bufferInfo["byteLength"].asUInt();
	ret.extras = bufferInfo["extras"];
	ret.name = bufferInfo["name"].asString();
	
	add( key, move( ret ) );
}

const BufferView& File::getBufferViewInfo( const std::string &name ) const
{
	auto found = mBufferViews.find( name );
	return found->second;
}
	
template<>
void File::add( const std::string &key, gltf::BufferView bufferView )
{
	mBufferViews[key] = move( bufferView );
}

void File::addBufferViewInfo( const std::string &key, const Json::Value &bufferViewInfo )
{
	CI_ASSERT( bufferViewInfo["byteOffset"].isNumeric() );
	CI_ASSERT( bufferViewInfo["buffer"].isString() );
	
	BufferView ret;
	auto bufferKey = bufferViewInfo["buffer"].asString();
	auto &buffer = mBuffers[bufferKey];
	ret.buffer = &buffer;
	ret.byteOffset = bufferViewInfo["byteOffset"].asUInt();
	ret.byteLength = bufferViewInfo["byteLength"].asUInt();
	ret.target = static_cast<BufferView::Target>( bufferViewInfo["target"].asUInt() );
	ret.name = bufferViewInfo["name"].asString();
	ret.extras = bufferViewInfo["extras"];
	
	add( key, ret );
}
	
const Camera& File::getCameraInfo( const std::string &key ) const
{
	auto found = mCameras.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Camera camera )
{
	mCameras[key] = move( camera );
}
	
void File::addCameraInfo( const std::string &key, const Json::Value &cameraInfo )
{
	CI_ASSERT( cameraInfo["type"].isString() );
	
	Camera ret;
	auto cameraType = cameraInfo["type"].asString();
	ret.type = cameraType == "perspective" ? Camera::Type::PERSPECTIVE : Camera::Type::ORTHOGRAPHIC;
	if ( ret.type == Camera::Type::PERSPECTIVE ) {
		auto &perspectiveInfo = cameraInfo["perspective"];
		
		CI_ASSERT( perspectiveInfo["yfov"].isNumeric() );
		CI_ASSERT( perspectiveInfo["znear"].isNumeric() );
		CI_ASSERT( perspectiveInfo["zfar"].isNumeric() );
		
		ret.aspectRatio = perspectiveInfo["aspectRatio"].asFloat();
		ret.yfov = perspectiveInfo["yfov"].asFloat();
		ret.znear = perspectiveInfo["znear"].asFloat();
		ret.zfar = perspectiveInfo["zfar"].asFloat();
		ret.camSpecificExtras = perspectiveInfo["extras"];
	}
	else if( ret.type == Camera::Type::ORTHOGRAPHIC ) {
		auto &orthographicInfo = cameraInfo["orthographic"];
		
		CI_ASSERT( orthographicInfo["xmag"].isNumeric() );
		CI_ASSERT( orthographicInfo["ymag"].isNumeric() );
		CI_ASSERT( orthographicInfo["znear"].isNumeric() );
		CI_ASSERT( orthographicInfo["zfar"].isNumeric() );
		
		ret.xmag = orthographicInfo["xmag"].asFloat();
		ret.ymag = orthographicInfo["ymag"].asFloat();
		ret.znear = orthographicInfo["znear"].asFloat();
		ret.zfar = orthographicInfo["zfar"].asFloat();
		ret.camSpecificExtras = orthographicInfo["extras"];
	}
	ret.name = cameraInfo["name"].asString();
	ret.extras = cameraInfo["extras"];
	
	add( key, move( ret ) );
}
	
const Image& File::getImageInfo( const std::string &key ) const
{
	auto found = mImages.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Image image )
{
	mImages[key] = move( image );
}
	
void File::addImageInfo( const std::string &key, const Json::Value &imageInfo )
{
	CI_ASSERT( imageInfo["uri"].isString() );
	
	Image ret;
	ret.uri = imageInfo["uri"].asString();
	ret.name = imageInfo["name"].asString();
	
	// in embedded use this to look at type
	size_t dataUri = ret.uri.find( "data:" );
	
	if( dataUri != std::string::npos  ) {
		auto binaryExt = imageInfo["extensions"]["KHR_binary_glTF"];
		std::string extension;
		ci::BufferRef buf;
		if( ! binaryExt.isNull() ) {
			auto binaryExt = imageInfo["extensions"]["KHR_binary_glTF"];
			auto bufferView = binaryExt["bufferView"].asString();
			// auto size = ivec2( binaryExt["width"].asUInt(), binaryExt["height"].asUInt() );
			extension = binaryExt["mimeType"].asString();
			auto &bufferViewInfo = mGltfTree["bufferViews"][bufferView];
			auto byteOffset = bufferViewInfo["byteOffset"].asUInt();
			auto byteLength = bufferViewInfo["byteLength"].asUInt();
			auto bufferName = bufferViewInfo["buffer"].asString();
			buf->setSize( byteLength );
			memcpy( buf->getData(), reinterpret_cast<uint8_t*>(mBuffer->getData()) + byteOffset, byteLength );
		}
		else {
			dataUri += 5; // past data
			auto beginning = ret.uri.find('/');
			auto end = ret.uri.find( ';' );
			auto typeStr = ret.uri.substr( beginning + 1, end );
			if( typeStr == "image/png" ) extension = "png";
			else if( typeStr == "image/jpeg" ) extension = "jpeg";
			auto dataBegin = ret.uri.find( ',' ) + 1;
			auto len = ret.uri.size() - dataBegin;
			buf = ci::BufferRef( new ci::Buffer( fromBase64( &ret.uri[dataBegin], len ) ) );
		}
		ret.imageSource = ci::loadImage( DataSourceBuffer::create( buf ), ImageSource::Options(), extension );
	}
	else
		ret.imageSource = loadImage( loadFile( mGltfPath / ret.uri ) );
	
	add( key, move( ret ) );
}
	
const Light& File::getLightInfo( const std::string &key ) const
{
	auto found = mLights.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Light light )
{
	mLights[key] = move( light );
}
	
void File::addLightInfo( const std::string &key, const Json::Value &val )
{
	CI_ASSERT( val["type"].isString() );
	
	Light ret;
	auto type = val["type"].asString();
	if( type == "ambient" )
		ret.type = Light::Type::AMBIENT;
	else if( type == "directional" )
		ret.type = Light::Type::DIRECTIONAL;
	else if( type == "point" )
		ret.type = Light::Type::POINT;
	else if( type == "spot" )
		ret.type = Light::Type::SPOT;
	else
		CI_ASSERT_MSG( false, "Light only supports the above types" );
	
	auto &lightTypeInfo = val[type];
	int i = 0;
	for( auto &colorVal : lightTypeInfo["color"] ) {
		ret.color[i++] = colorVal.asFloat();
	}
	ret.constantAttenuation = lightTypeInfo["constantAttenuation"].asFloat();
	if( ret.type == Light::Type::POINT || ret.type == Light::Type::SPOT ) {
		ret.distance = lightTypeInfo["distance"].asFloat();
		if( lightTypeInfo["linearAttenuation"].isNumeric() )
			ret.linearAttenuation = lightTypeInfo["linearAttenutation"].asFloat();
		if( lightTypeInfo["quadraticAttenuation"].isNumeric() )
			ret.quadraticAttenuation = lightTypeInfo["quadraticAttenuation"].asFloat();
		if( ret.type == Light::Type::SPOT ) {
			if( lightTypeInfo["falloffAngle"].isNumeric() )
				ret.falloffAngle = lightTypeInfo["falloffAngle"].asFloat();
			ret.falloffExponent = lightTypeInfo["falloffExponent"].asFloat();
		}
	}
	ret.name = key;
	add( key, move( ret ) );
}
	
const Material& File::getMaterialInfo( const std::string &key ) const
{
	auto found = mMaterials.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Material material )
{
	mMaterials[key] = move( material );
}
	
void File::addMaterialInfo( const std::string &key, const Json::Value &materialInfo )
{
	Material ret;
	
	auto &materialExt = materialInfo["extensions"]["KHR_materials_common"];
	auto &material = materialExt.isNull() ? materialInfo : materialExt;
	
	auto techKey = material["technique"].asString();
	auto &technique = mTechniques[techKey];
	ret.technique = &technique;
	
	auto &values = material["values"];
	auto valueKeys = material["values"].getMemberNames();
	int i;
	for( auto &valueKey : valueKeys ) {
		if( valueKey == "ambient" ) {
			auto &ambient = values[valueKey];
			i = 0;
			for( auto &ambVal : ambient ) {
				ret.ambient[i++] = ambVal.asFloat();
			}
		}
		else if( valueKey == "diffuse" || valueKey == "specular" || valueKey == "emission" ) {
			auto &source = values[valueKey];
			Material::Source src;
			
			if( valueKey == "diffuse" )
				src.type = Material::Source::Type::DIFFUSE;
			else if( valueKey == "specular" )
				src.type = Material::Source::Type::SPECULAR;
			else if( valueKey == "emission" )
				src.type = Material::Source::Type::EMISSION;
			else
				CI_ASSERT_MSG( false, "Only the above types are supported" );
			
			if( source.isArray() ) {
				i = 0;
				for( auto & sourceVal : source ) {
					src.color[i++] = sourceVal.asFloat();
				}
			}
			else if( source.isString() ) {
				auto sourceKey = source.asString();
				auto &texture = mTextures[sourceKey];
				src.texture = &texture;
			}
			
			ret.sources.emplace_back( move( src ) );
		}
		else if( valueKey == "shininess" ) {
			ret.shininess = values[valueKey].asFloat();
		}
		else if( valueKey == "doubleSided" ) {
			ret.doubleSided = values[valueKey].asBool();
		}
		else if( valueKey == "transparency" ) {
			ret.transparency = values[valueKey].asFloat();
		}
		else if( valueKey == "transparent" ) {
			ret.transparent = values[valueKey].asBool();
		}
		else if( valueKey == "jointCount" ) {
			ret.jointCount = values[valueKey].asUInt();
		}
		else {
			ret.values[valueKey] = values[valueKey];
		}
	}
	
	ret.name = materialInfo["name"].asString();
	ret.extras = material["extras"];
	
	add( key, move( ret ) );
}
	
const Mesh& File::getMeshInfo( const std::string &key ) const
{
	auto found = mMeshes.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Mesh mesh )
{
	mMeshes[key] = move( mesh );
}
	
void File::addMeshInfo( const std::string &key, const Json::Value &meshInfo )
{
	Mesh ret;
	for( auto &primitive : meshInfo["primitives"] ) {
		CI_ASSERT( primitive["material"].isString() );
		
		Mesh::Primitive meshPrim;
		auto materialKey = primitive["material"].asString();
		auto &material = mMaterials[materialKey];
		meshPrim.material = &material;
		auto indicesAccessor = primitive["indices"].asString();
		auto &accessor = mAccessors[indicesAccessor];
		meshPrim.indices = &accessor;
		meshPrim.primitive = primitive["mode"].asUInt();
		meshPrim.extras = primitive["extras"];
		
		auto &attributes = primitive["attributes"];
		auto attribNames = attributes.getMemberNames();
		for( int i = 0; i < attribNames.size(); i++ ) {
			auto &attribName = attribNames[i];
			Mesh::Primitive::AttribAccessor attrib;
			attrib.attrib = Mesh::getAttribEnum( attribName );
			auto accessorKey = attributes[attribName].asString();
			auto &attribAccessor = mAccessors[accessorKey];
			attrib.accessor = &attribAccessor;
			meshPrim.attributes.emplace_back( move( attrib ) );
		}
		
		ret.primitives.emplace_back( move( meshPrim ) );
	}
	ret.name = meshInfo["name"].asString();
	ret.extras = meshInfo["extras"];
	
	add( key, move( ret ) );
}
	
const Node& File::getNodeInfo( const std::string &key ) const
{
	auto found = mNodes.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Node node )
{
	mNodes[key] = move( node );
}
	
void File::addNodeInfo( const std::string &key, const Json::Value &nodeInfo )
{
	Node ret;
	
	if( ! nodeInfo["matrix"].isNull() ) {
		auto matrix = nodeInfo["matrix"];
		ret.transformMatrix.reserve( 16 );
		for( auto & matVal : matrix ) {
			ret.transformMatrix.push_back( matVal.asFloat() );
		}
	}
	else {
		if( ! nodeInfo["translation"].isNull() ) {
			auto transArray = nodeInfo["translation"];
			ret.translation.reserve( 3 );
			for( auto & transVal : transArray )
				ret.translation.push_back( transVal.asFloat() );
		}
		if( ! nodeInfo["rotation"].isNull() ) {
			auto rotArray = nodeInfo["rotation"];
			ret.rotation.reserve( 4 );
			for( auto & rotVal : rotArray )
				ret.rotation.push_back( rotVal.asFloat() );
		}
		if( ! nodeInfo["scale"].isNull() ) {
			auto & scaleArray = nodeInfo["scale"];
			ret.scale.reserve( 3 );
			for( auto & scaleVal : scaleArray )
				ret.scale.push_back( scaleVal.asFloat() );
		}
	}
	
	if( ! nodeInfo["extensions"].isNull() ) {
		auto &ext = nodeInfo["extensions"];
		if( ! ext["KHR_materials_common"].isNull() ) {
			auto lightKey = ext["KHR_materials_common"]["light"].asString();
			auto &light = mLights[lightKey];
			ret.light = &light;
		}
	}
	else if( ! nodeInfo["camera"].isNull() ) {
		auto cameraKey = nodeInfo["camera"].asString();
		auto &camera = mCameras[cameraKey];
		ret.camera = &camera;
	}
	else if( ! nodeInfo["jointName"].isNull() ) {
		ret.jointName = nodeInfo["jointName"].asString();
	}
	else {
		if( ! nodeInfo["meshes"].isNull() ) {
			for( auto &meshInfo : nodeInfo["meshes"] ) {
				auto meshKey = meshInfo.asString();
				auto &mesh = mMeshes[meshKey];
				ret.meshes.push_back( &mesh );
			}
		}
		if( ! nodeInfo["skin"].isNull() ) {
			auto skinKey = nodeInfo["skin"].asString();
			auto &skin = mSkins[skinKey];
			ret.skin = &skin;
		}
		if( ! nodeInfo["skeletons"].isNull() ) {
			for( auto &skeletonInfo : nodeInfo["skeletons"] ) {
				auto skeletonRootKey = skeletonInfo.asString();
				auto &skeleton = mNodes[skeletonRootKey];
				ret.skeletons.push_back( &skeleton );
			}
		}
	}
	
//	if( ! nodeInfo["children"].isNull() ) {
//		for( auto &childInfo : nodeInfo["children"] ) {
//			auto childKey
//		}
//	}
	
	ret.name = nodeInfo["name"].asString();
	ret.extras = nodeInfo["extras"];
	
	add( key, move( ret ) );
}
	
const Program& File::getProgramInfo( const std::string &key ) const
{
	auto found = mPrograms.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Program program )
{
	mPrograms[key] = move( program );
}
	
void File::addProgramInfo( const std::string &key, const Json::Value &programInfo )
{
	CI_ASSERT( programInfo["vertexShader"].isString() );
	CI_ASSERT( programInfo["fragmentShader"].isString() );
	
	Program ret;
	auto vertShaderKey = programInfo["vertexShader"].asString();
	auto &vertShader = mShaders[vertShaderKey];
	ret.vert = &vertShader;
	auto fragShaderKey = programInfo["fragmentShader"].asString();
	auto &fragShader = mShaders[fragShaderKey];
	ret.frag = &fragShader;
	
	auto &attributes = programInfo["attributes"];
	for( auto & attribute : attributes )
		ret.attributes.push_back( attribute.asString() );
	
	ret.name = programInfo["name"].asString();
	ret.extras = programInfo["extras"].asString();
	
	add( key, move( ret ) );
}

const Sampler& File::getSamplerInfo( const std::string &key ) const
{
	auto found = mSamplers.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Sampler sampler )
{
	mSamplers[key] = move( sampler );
}
	
void File::addSamplerInfo( const std::string &key, const Json::Value &samplerInfo )
{
	Sampler ret;
	
	if( samplerInfo["magFilter"].isNumeric() )
		ret.magFilter = samplerInfo["magFilter"].asUInt();
	if( samplerInfo["minFilter"].isNumeric() )
		ret.minFilter = samplerInfo["minFilter"].asUInt();
	if( samplerInfo["wrapS"].isNumeric() )
		ret.wrapS = samplerInfo["wrapS"].asUInt();
	if( samplerInfo["wrapT"].isNumeric() )
		ret.wrapT = samplerInfo["wrapT"].asUInt();
	
	ret.name = samplerInfo["name"].asString();
	ret.extras = samplerInfo["extras"];
	
	add( key, move( ret ) );
}
	
const Scene& File::getSceneInfo( const std::string &key ) const
{
	auto found = mScenes.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Scene scene )
{
	mScenes[key] = move( scene );
}
	
void File::addSceneInfo( const std::string &key, const Json::Value &sceneInfo )
{
	Scene ret;
	
	auto &nodes = sceneInfo["nodes"];
	ret.nodes.resize( nodes.size() );
	int i = 0;
	for( auto & node : nodes ) {
		auto nodeKey = node.asString();
		auto &nodeInst = mNodes[nodeKey];
		ret.nodes[i++] = &nodeInst;
	}
	
	ret.name = sceneInfo["names"].asString();
	ret.extras = sceneInfo["extras"];
	
	add( key, move( ret ) );
}
	
const Shader& File::getShaderInfo( const std::string &key ) const
{
	auto found = mShaders.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Shader shader )
{
	mShaders[key] = move( shader );
}
	
void File::addShaderInfo( const std::string &key, const Json::Value &shaderInfo )
{
	CI_ASSERT( shaderInfo["uri"].isString() );
	CI_ASSERT( shaderInfo["type"].isNumeric() );
	
	Shader ret;
	
	auto uri = shaderInfo["uri"].asString();
	
	// either it's embeded, binary, or separate
	
	auto pos = uri.find(",");
	if( pos != std::string::npos ) {
		if( pos + 1 == uri.size() && hasExtension( "KHR_binary_glTF" ) ) {
			auto binaryExt = shaderInfo["extensions"]["KHR_binary_glTF"];
			auto bufferView = binaryExt["bufferView"].asString();
			auto &bufferViewInfo = mGltfTree["bufferViews"][bufferView];
			auto offset = bufferViewInfo["byteOffset"].asUInt();
			auto length = bufferViewInfo["byteLength"].asUInt();
			ret.source.append( reinterpret_cast<char*>( mBuffer->getData() ) + offset, length );
			ret.uri = uri;
		}
		else {
			auto data = uri.substr( pos + 1, uri.size() );
			auto buffer = fromBase64( data );
			ret.source = std::string( static_cast<char*>(buffer.getData()), buffer.getSize() );
		}
	}
	else {
		ret.source = loadString( loadFile( mGltfPath / uri ) );
	}
	ret.type = static_cast<Shader::Type>( shaderInfo["type"].asUInt() );
	ret.uri = shaderInfo["uri"].asString();
	ret.name = shaderInfo["name"].asString();
	ret.extras = shaderInfo["extras"];
	
	add( key, move( ret ) );
}
	
const Skin& File::getSkinInfo( const std::string &key ) const
{
	auto found = mSkins.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Skin skin )
{
	mSkins[key] = move( skin );
}
	
void File::addSkinInfo( const std::string &key, const Json::Value &skinInfo )
{
	CI_ASSERT( skinInfo["inverseBindMatrices"].isString() );
	CI_ASSERT( ! skinInfo["jointNames"].isNull() );
	
	Skin ret;
	
	auto accessorKey = skinInfo["inverseBindMatrices"].asString();
	auto &accessor = mAccessors[accessorKey];
	ret.inverseBindMatrices = &accessor;
	for( auto &jointName : skinInfo["jointNames"] ) {
		auto &joint = mNodes[jointName.asString()];
		ret.joints.push_back( &joint );
	}
	if( ! skinInfo["bindShapeMatrix"].isNull() ) {
		auto &bindShapeMatrix = skinInfo["bindShapeMatrix"];
		int i = 0;
		for( auto &bind : bindShapeMatrix ) {
			ret.bindShapeMatrix[i / 4][i % 4] = bind.asFloat();
			i++;
		}
	}
	
	ret.name = skinInfo["name"].asString();
	ret.extras = skinInfo["extras"];
	
	add( key, move( ret ) );
}

const Technique& File::getTechniqueInfo( const std::string &key ) const
{
	auto found = mTechniques.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Technique technique )
{
	mTechniques[key] = move( technique );
}
	
void File::addTechniqueInfo( const std::string &key, const Json::Value &techniqueInfo )
{
	CI_ASSERT( techniqueInfo["program"].isString() );
	
	Technique ret;
	auto programKey = techniqueInfo["program"].asString();
	auto &program = mPrograms[programKey];
	ret.program = &program;
	
	auto &attribs = techniqueInfo["attributes"];
	auto attribNames = attribs.getMemberNames();
	ret.attributes.reserve( attribNames.size() );
	for( int i = 0; i < attribNames.size(); i++ ) {
		auto &attribName = attribNames[i];
		auto pair = make_pair( attribName, attribs[attribName].asString() );
		ret.attributes.emplace_back( move( pair ) );
	}
	
	auto &uniforms = techniqueInfo["uniforms"];
	auto uniformNames = uniforms.getMemberNames();
	ret.attributes.reserve( uniformNames.size() );
	for( int i = 0; i < uniformNames.size(); i++ ) {
		auto &uniformName = uniformNames[i];
		auto pair = make_pair( uniformName, attribs[uniformName].asString() );
		ret.uniforms.emplace_back( move( pair ) );
	}
	
	if( ! techniqueInfo["states"].isNull() ) {
		auto &states = techniqueInfo["states"];
		if( ! states["enable"].isNull() ) {
			auto &enables = states["enable"];
			ret.states.enables.reserve( enables.size() );
			std::transform( enables.begin(), enables.end(), std::back_inserter( ret.states.enables ),
						   []( const Json::Value &val ){ return val.asUInt(); } );
		}
		if( ! states["functions"].isNull() ) {
			auto &functions = states["functions"];
			int i = 0;
			if( ! functions["blendColor"].isNull() ) {
				i = 0;
				for( auto &color : functions["blendColor"] )
					ret.states.functions.blendColor[i++] = color.asFloat();
			}
			if( ! functions["blendEquationSeparate"].isNull() ) {
				i = 0;
				for( auto &equation : functions["blendEquationSeparate"] )
					ret.states.functions.blendEquationSeparate[i++] = equation.asUInt();
			}
			if( ! functions["blendFuncSeparate"].isNull() ) {
				i = 0;
				for( auto &func : functions["blendFuncSeparate"] )
					ret.states.functions.blendFuncSeparate[i++] = func.asUInt();
			}
			if( ! functions["colorMask"].isNull() ) {
				i = 0;
				for( auto &mask : functions["colorMask"] )
					ret.states.functions.colorMask[i++] = mask.asBool();
			}
			if( ! functions["depthRange"].isNull() ) {
				i = 0;
				for( auto &range : functions["depthRange"] )
					ret.states.functions.depthRange[i++] = range.asFloat();
			}
			if( ! functions["polygonOffset"].isNull() ) {
				i = 0;
				for( auto &offset : functions["polygonOffset"] )
					ret.states.functions.polygonOffset[i++] = offset.asFloat();
			}
			if( ! functions["scissor"].isNull() ) {
				i = 0;
				for( auto &scissor : functions["scissor"] )
					ret.states.functions.scissor[i++] = scissor.asFloat();
			}
			if( ! functions["lineWidth"].isNull() )
				ret.states.functions.lineWidth = functions["lineWidth"].asFloat();
			if( ! functions["cullFace"].isNull() )
				ret.states.functions.cullFace = functions["cullFace"].asUInt();
			if( ! functions["depthFunc"].isNull() )
				ret.states.functions.depthFunc = functions["depthFunc"].asUInt();
			if( ! functions["frontFace"].isNull() )
				ret.states.functions.frontFace = functions["frontFace"].asUInt();
			if( ! functions["depthMask"].isNull() )
				ret.states.functions.depthMask = functions["depthMask"].asBool();
			ret.states.functions.extras = functions["extras"];
		}
		ret.states.extras = states["extras"];
	}
	
	// Parameters Excavation...
	auto &parameters = techniqueInfo["parameters"];
	ret.parameters.reserve( parameters.size() );
	for ( auto & param : parameters ) {
		CI_ASSERT( ! param["type"].isNull() );
		Technique::Parameter techParam;
		techParam.type = param["type"].asUInt();
		if( ! param["count"].isNull() )
			techParam.count = param["count"].asUInt();
		if( ! param["node"].isNull() ) {
			auto nodeKey = param["node"].asString();
			auto &node = mNodes[nodeKey];
			techParam.node = &node;
		}
		if( ! param["semantic"].isNull() )
			techParam.semantic = param["semantic"].asString();
		techParam.name = param["name"].asString();
		techParam.extras = param["extras"];
		ret.parameters.emplace_back( move( techParam ) );
	}
	
	ret.name = techniqueInfo["name"].asString();
	ret.extras = techniqueInfo["extras"];
	
	add( key, move( ret ) );
}
	
const Texture& File::getTextureInfo( const std::string &key ) const
{
	auto found = mTextures.find( key );
	return found->second;
}
	
template<>
void File::add( const std::string &key, Texture texture )
{
	mTextures[key] = move( texture );
}

void File::addTextureInfo( const std::string &key, const Json::Value &textureInfo )
{
	CI_ASSERT( textureInfo["sampler"].isString() );
	CI_ASSERT( textureInfo["source"].isString() );
	
	Texture ret;
	auto imageKey = textureInfo["source"].asString();
	auto &image = mImages[imageKey];
	ret.image = &image;
	auto samplerKey = textureInfo["sampler"].asString();
	auto &sampler = mSamplers[samplerKey];
	ret.sampler = &sampler;
	if( textureInfo["target"].isNumeric() )
		ret.target = textureInfo["target"].asUInt();
	if( textureInfo["format"].isNumeric() )
		ret.format = textureInfo["format"].asUInt();
	if( textureInfo["internalFormat"].isNumeric() )
		ret.internalFormat = textureInfo["internalFormat"].asUInt();
	if( textureInfo["type"].isNumeric() )
		ret.type = textureInfo["type"].asUInt();
	
	ret.name = textureInfo["name"].asString();
	ret.extras = textureInfo["extras"];
	
	add( key, move( ret ) );
}
	
Skeleton::AnimRef File::createSkeletonAnim( const SkeletonRef &skeleton ) const
{
	std::vector<TransformClip> skeletonClips;
	skeletonClips.reserve( skeleton->getNumJoints() );
	for( auto &boneName : skeleton->getJointNames() ) {
		auto found = std::find_if( mAnimations.begin(), mAnimations.end(),
		[boneName]( const std::pair<std::string, gltf::Animation> &animation ){
			  return animation.second.target == boneName;
		});
		if( found != mAnimations.end() ) {
			auto params = found->second.getParameters();
			skeletonClips.emplace_back( gltf::Animation::createTransformClip( params ) );
		}
		else {
			CI_ASSERT_MSG( false, "Need to figure out how to handle this case" );
		}
	}
	auto ret = make_shared<Skeleton::Anim>( std::move( skeletonClips ) );
	return ret;
}
	
Skeleton::AnimCombinedRef File::createSkeletonAnimCombined( const SkeletonRef &skeleton ) const
{
	std::vector<TransformClip> skeletonClips;
	skeletonClips.reserve( skeleton->getNumJoints() );
	for( auto &boneName : skeleton->getJointNames() ) {
		auto found = std::find_if( mAnimations.begin(), mAnimations.end(),
								  [boneName]( const std::pair<std::string, gltf::Animation> &animation ){
									  return animation.second.target == boneName;
								  });
		if( found != mAnimations.end() ) {
			auto params = found->second.getParameters();
			skeletonClips.emplace_back( gltf::Animation::createTransformClip( params ) );
		}
		else {
			CI_ASSERT_MSG( false, "Need to figure out how to handle this case" );
		}
	}
	auto ret = make_shared<Skeleton::AnimCombined>( std::move( skeletonClips ) );
	return ret;
}
	
Skeleton::AnimSeparatedRef File::createSkeletonAnimSeparated( const SkeletonRef &skeleton ) const
{
	std::vector<TransformClip> skeletonClips;
	skeletonClips.reserve( skeleton->getNumJoints() );
	for( auto &boneName : skeleton->getJointNames() ) {
		auto found = std::find_if( mAnimations.begin(), mAnimations.end(),
								  [boneName]( const std::pair<std::string, gltf::Animation> &animation ){
									  return animation.second.target == boneName;
								  });
		if( found != mAnimations.end() ) {
			auto params = found->second.getParameters();
			skeletonClips.emplace_back( gltf::Animation::createTransformClip( params ) );
		}
		else {
			CI_ASSERT_MSG( false, "Need to figure out how to handle this case" );
		}
	}
	auto ret = make_shared<Skeleton::AnimSeparated>( std::move( skeletonClips ) );
	return ret;
	
}

CameraOrtho File::getOrthoCameraByName( const std::string &name )
{
	Camera cam = getCameraInfo( name );
	if( cam.type != Camera::Type::ORTHOGRAPHIC ) throw "This should be orthographic but it's not";
	
	//TODO: This is most likely wrong need to change it.
	CameraOrtho ret( -cam.xmag, cam.xmag, -cam.ymag, cam.ymag, cam.znear, cam.zfar);
	
	return ret;
}

CameraPersp File::getPerspCameraByName( const std::string &name )
{
	Camera cam = getCameraInfo( name );
	if( cam.type != Camera::Type::PERSPECTIVE ) throw "This should be perspective but it's not";
	
	CameraPersp ret; //( app->getWindowWidth(), app->getWindowHeight(), cam.yfov, cam.znear, cam.zfar );
	ret.setPerspective( cam.aspectRatio, cam.yfov, cam.znear, cam.zfar );

	return ret;
}
	
ci::geom::Primitive Mesh::convertToPrimitive( GLenum primitive )
{
	switch (primitive) {
		case GL_LINES: return ci::geom::LINES; break;
		case GL_LINE_STRIP:	return ci::geom::LINE_STRIP; break;
		case GL_TRIANGLES: return ci::geom::TRIANGLES; break;
		case GL_TRIANGLE_STRIP: return ci::geom::TRIANGLE_STRIP; break;
		case GL_TRIANGLE_FAN: return ci::geom::TRIANGLE_FAN; break;
		default: CI_LOG_E("Don't know this primitive"); return (ci::geom::Primitive)-1; break;
	}
}
	
ci::geom::Attrib Mesh::getAttribEnum( const std::string &attrib )
{
	using namespace ci::geom;
	if( attrib == "POSITION" )			return Attrib::POSITION;
	else if( attrib == "NORMAL" )		return Attrib::NORMAL;
	else if( attrib == "TEXCOORD_0" )	return Attrib::TEX_COORD_0;
	else if( attrib == "TEXCOORD_1" )	return Attrib::TEX_COORD_1;
	else if( attrib == "TEXCOORD_2" )	return Attrib::TEX_COORD_2;
	else if( attrib == "TEXCOORD_3" )	return Attrib::TEX_COORD_3;
	else if( attrib == "COLOR" )		return Attrib::COLOR;
	else if( attrib == "JOINT" )		return Attrib::BONE_INDEX;
	else if( attrib == "JOINTMATRIX" ) {
		CI_LOG_W( "UNDEFINED Attib JOINTMATRIX Using CUSTOM_0" );
		return Attrib::CUSTOM_0;
	}
	else if( attrib == "WEIGHT" )		return Attrib::BONE_WEIGHT;
	else								return Attrib::NUM_ATTRIBS;
}
	
ci::gl::UniformSemantic Technique::getUniformEnum( const std::string &uniform )
{
	auto & u = uniform;
	using namespace ci::gl;
	if( u == "MODEL" )								return UniformSemantic::UNIFORM_MODEL_MATRIX;
	else if( u == "VIEW" )							return UniformSemantic::UNIFORM_VIEW_MATRIX;
	else if( u == "PROJECTION" )					return UniformSemantic::UNIFORM_PROJECTION_MATRIX;
	else if( u == "MODELVIEW" )						return UniformSemantic::UNIFORM_MODEL_VIEW;
	else if( u == "MODELVIEWPROJECTION" )			return UniformSemantic::UNIFORM_MODEL_VIEW_PROJECTION;
	else if( u == "MODELINVERSE" )					return UniformSemantic::UNIFORM_MODEL_MATRIX_INVERSE;
	else if( u == "VIEWINVERSE" )					return UniformSemantic::UNIFORM_VIEW_MATRIX_INVERSE;
	else if( u == "PROJECTIONINVERSE" )				return UniformSemantic::UNIFORM_PROJECTION_MATRIX_INVERSE;
	else if( u == "MODELVIEWINVERSE" )				return UniformSemantic::UNIFORM_MODEL_VIEW;
	else if( u == "MODELVIEWPROJECTIONINVERSE" )	return UniformSemantic::UNIFORM_MODEL_VIEW_PROJECTION;
	else if( u == "MODELINVERSETRANSPOSE" )			return UniformSemantic::UNIFORM_MODEL_MATRIX_INVERSE;
	else if( u == "MODELVIEWINVERSETRANSPOSE" )		return UniformSemantic::UNIFORM_MODEL_VIEW_INVERSE_TRANSPOSE;
	else if( u == "VIEWPORT" )						return UniformSemantic::UNIFORM_VIEWPORT_MATRIX;
	else return (UniformSemantic)-1;
}
	
uint8_t Accessor::getNumComponents() const
{
	switch( dataType ) {
		case Accessor::DataType::SCALAR: return 1;
		case Accessor::DataType::VEC2: return 2;
		case Accessor::DataType::VEC3: return 3;
		case Accessor::DataType::VEC4:
		case Accessor::DataType::MAT2: return 4;
		case Accessor::DataType::MAT3: return 9;
		case Accessor::DataType::MAT4: return 12;
	}
}

uint8_t Accessor::getNumBytesForComponentType() const
{
	switch (componentType) {
		case Accessor::ComponentType::BYTE:
		case Accessor::ComponentType::UNSIGNED_BYTE:
			return 1;
			break;
		case Accessor::ComponentType::SHORT: // SHORT
		case Accessor::ComponentType::UNSIGNED_SHORT: // UNSIGNED_SHORT
			return 2;
			break;
		case Accessor::ComponentType::FLOAT: // FLOAT
			return 4;
			break;
		default: {
			CI_LOG_E("ERROR: That enum doesn't have a dimmension/size.");
			return 0;
		}
			break;
	}
}
	
void* Accessor::getDataPtr() const
{
	const auto &buffer = bufferView->buffer;
	return reinterpret_cast<uint8_t*>(buffer->getBuffer()->getData()) + bufferView->byteOffset + byteOffset;
}
	
const Node* Node::getChild( size_t index ) const
{
	return children[index];
}

const Node* Node::getChild( const std::string &nodeName ) const
{
	const Node* ret = nullptr;
	auto endIt = end( children );
	auto found = std::find_if( begin( children ), endIt,
	[nodeName]( const Node *node ){
		return nodeName == node->name;
	});
	if( found != endIt ) {
		ret = *found;
	}
	return ret;
}
	
SkeletonRef Skin::createSkeleton() const
{
	auto matricesPtr = reinterpret_cast<ci::mat4*>( inverseBindMatrices->getDataPtr() );
	auto numJoints = joints.size();
	std::vector<std::string> jointNames;
	jointNames.reserve( numJoints );
	std::vector<Skeleton::Joint> jointsContainer;
	jointsContainer.reserve( numJoints );
	for( int i = 0; i < numJoints; i++ ) {
		uint8_t parentId;
		// if this joint is the root.
		if( i == 0 )
			parentId = 0xFF;
		else {
			auto begIt = begin( jointNames );
			auto foundIt = std::find( begIt, end( jointNames ), joints[i]->parent->name );
			auto distance = std::distance( begIt, foundIt );
			parentId = distance;
		}
		CI_ASSERT( !  joints[i]->jointName.empty() );
		jointNames.emplace_back(  joints[i]->jointName );
		jointsContainer.emplace_back( parentId, jointNames.size() - 1, *matricesPtr++ );
	}
	auto ret = std::make_shared<Skeleton>( std::move( jointsContainer ), std::move( jointNames ) );
	return ret;
}
	
std::vector<Animation::ParameterData> Animation::getParameters() const
{
	std::vector<Animation::ParameterData> ret;
	ret.reserve( parameters.size() + 1 );
	
	// Initialize times for keyframes
	CI_ASSERT( timeAccessor->dataType == Accessor::DataType::SCALAR );
	auto totalKeyFrames = timeAccessor->count;
	auto dataPtr = timeAccessor->getDataPtr();
	
	Animation::ParameterData time{ "TIME", 1, std::vector<float>( totalKeyFrames ) };
	memcpy( time.data.data(), dataPtr, totalKeyFrames * sizeof( float ) );
	ret.emplace_back( move( time ) );
	
	for( auto & param : parameters ) {
		const auto accessor = param.accessor;
		auto numComponents = accessor->getNumComponents();
		
		CI_ASSERT( totalKeyFrames == accessor->count );
		auto dataPtr = accessor->getDataPtr();
		
		Animation::ParameterData parameter{ param.parameter, numComponents, std::vector<float>( totalKeyFrames * numComponents ) };
		memcpy( parameter.data.data(), dataPtr, accessor->count * numComponents * sizeof( float ) );
		ret.emplace_back( move( parameter ) );
	}

	return ret;
}
	
TransformClip Animation::createTransformClip( const std::vector<ParameterData> &paramData )
{
	const std::vector<float> *timeData = nullptr, *scaleData = nullptr,
							 *transData = nullptr, *rotData = nullptr;
	for( auto &param : paramData )
		if( param.paramName == "TIME" )
			timeData = &param.data;
		else if( param.paramName == "rotation" )
			rotData = &param.data;
		else if( param.paramName == "scale" )
			scaleData = &param.data;
		else if( param.paramName == "translation" )
			transData = &param.data;
	
	auto totalKeyFrames = timeData->size();
	
	std::vector<std::pair<double, ci::quat>> rotationKeyframes( totalKeyFrames );
	std::vector<std::pair<double, ci::vec3>> translationKeyframes( totalKeyFrames ),
											 scaleKeyframes( totalKeyFrames, { 0.0, vec3( 1 ) } );
	
	for( int i = 0, end = totalKeyFrames; i < end; i++ ) {
		auto time = (*timeData)[i];
		translationKeyframes[i].first = time;
		rotationKeyframes[i].first = time;
		scaleKeyframes[i].first = time;
		if( transData != nullptr ) {
			auto translation = *reinterpret_cast<const ci::vec3*>( &(*transData)[i*3] );
			translationKeyframes[i].second = translation;
		}
		if( rotData != nullptr ) {
			auto rotation = *reinterpret_cast<const ci::quat*>( &(*rotData)[i*4] );
			rotationKeyframes[i].second = rotation;
		}
		if( scaleData != nullptr ) {
			auto scale = *reinterpret_cast<const ci::vec3*>( &(*scaleData)[i*3] );
			scaleKeyframes[i].second = scale;
		}
	}
	
	TransformClip ret( std::move( translationKeyframes ),
					   std::move( rotationKeyframes ),
					   std::move( scaleKeyframes ) );
	return ret;
}
	
Clip<ci::vec3>	Animation::createTranslationClip( const std::vector<ParameterData> &paramData )
{
	const std::vector<float> *timeData = nullptr, *transData = nullptr;
	for( auto &param : paramData )
		if( param.paramName == "TIME" )
			timeData = &param.data;
		else if( param.paramName == "translation" )
			transData = &param.data;
	
	std::vector<std::pair<double, ci::vec3>> transformKeyFrames( timeData->size() );
	
	int i = 0;
	for( auto & transformKeyFrame : transformKeyFrames ) {
		transformKeyFrame.first = (*timeData)[i];
		auto translation = *reinterpret_cast<const ci::vec3*>( &(*transData)[i*3] );
		transformKeyFrame.second = translation;
		i++;
	}
	
	Clip<ci::vec3> ret( move( transformKeyFrames ) );
	return ret;
}

Clip<ci::vec3>	Animation::createScaleClip( const std::vector<ParameterData> &paramData )
{
	const std::vector<float> *timeData = nullptr, *scaleData = nullptr;
	for( auto &param : paramData )
		if( param.paramName == "TIME" )
			timeData = &param.data;
		else if( param.paramName == "scale" )
			scaleData = &param.data;
	
	std::vector<std::pair<double, ci::vec3>> transformKeyFrames( timeData->size() );
	
	int i = 0;
	for( auto & transformKeyFrame : transformKeyFrames ) {
		transformKeyFrame.first = (*timeData)[i];
		auto scale = *reinterpret_cast<const ci::vec3*>( &(*scaleData)[i*3] );
		transformKeyFrame.second = scale;
		i++;
	}
	
	Clip<ci::vec3> ret( move( transformKeyFrames ) );
	return ret;
}

Clip<ci::quat>	Animation::createRotationClip( const std::vector<ParameterData> &paramData )
{
	const std::vector<float> *timeData = nullptr, *rotData = nullptr;
	for( auto &param : paramData )
		if( param.paramName == "TIME" )
			timeData = &param.data;
		else if( param.paramName == "rotation" )
			rotData = &param.data;
	
	std::vector<std::pair<double, ci::quat>> transformKeyFrames( timeData->size() );
	
	int i = 0;
	for( auto & transformKeyFrame : transformKeyFrames ) {
		transformKeyFrame.first = (*timeData)[i];
		auto rotation = *reinterpret_cast<const ci::quat*>( &(*rotData)[i*4] );
		transformKeyFrame.second = rotation;
		i++;
	}
	
	Clip<ci::quat> ret( move( transformKeyFrames ) );
	return ret;
}
	
ci::mat4 Node::getTransformMatrix() const
{
	ci::mat4 ret;
	if( ! transformMatrix.empty() )
		ret = glm::make_mat4( transformMatrix.data() );
	return ret;
}

ci::vec3 Node::getTranslation() const
{
	ci::vec3 ret;
	if( ! translation.empty() )
		ret = glm::make_vec3( translation.data() );
	return ret;
}

ci::quat Node::getRotation() const
{
	ci::quat ret;
	if( ! rotation.empty() )
		ret = glm::make_quat( rotation.data() );
	return ret;
}

ci::vec3 Node::getScale() const
{
	ci::vec3 ret( 1.0f );
	if( ! scale.empty() )
		ret = glm::make_vec3( scale.data() );
	return ret;
}
	
void Node::outputToConsole( std::ostream &os, uint8_t tabAmount ) const
{
	using std::endl;
	os << "Name: " << name << endl;
	if( camera != nullptr )
		os << "Camera: " << camera->name << endl;
	else if( light != nullptr )
		os << "Light: " << light->name << endl;
	else if( ! jointName.empty() )
		os << "JointName: " << jointName << endl;
	else {
		if( ! meshes.empty() ) {
			os << "Meshes: " << endl;
			for( auto &mesh : meshes )
				os << "\t" << mesh->name << endl;
		}
		if( skin != nullptr )
			os << "Skin: " << skin->name << endl;
		if( ! skeletons.empty() ) {
			os << "Skeletons: " << endl;
			for( auto &skeleton : skeletons )
				os << "\t" << skeleton->name << endl;
		}
	}
	os << "Transform:" << endl;
	if( ! transformMatrix.empty() ) {
		os << "\tMatrix: [";
		int i = 0;
		for( auto &val : transformMatrix )
			os << " " << val << (i++ < 15 ? "," : " ]");
		os << endl;
	}
	else {
		if( ! translation.empty() ) {
			os << "\tTranslation: [";
			int i = 0;
			for( auto & val : translation )
				os << " " << val << (i++ < 2 ? "," : " ]");
			os << endl;
		}
		if( ! rotation.empty() ) {
			os << "\tRotation: [";
			int i = 0;
			for( auto & val : rotation )
				os << " " << val << (i++ < 3 ? "," : " ]");
			os << endl;
		}
		if( ! scale.empty() ) {
			os << "\tScale: [";
			int i = 0;
			for( auto & val : scale )
				os << " " << val << (i++ < 2 ? "," : " ]");
			os << endl;
		}
	}
	os << endl;
}

std::ostream& operator<<( std::ostream &lhs, const File &rhs )
{
	
	return lhs;
}
	
std::ostream& operator<<( std::ostream &lhs, const Accessor &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Animation &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Asset &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const BufferView &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Buffer &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Camera &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Image &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Material &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Mesh &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Node &rhs )
{
	rhs.outputToConsole( lhs, 0 );
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Program &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Sampler &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Scene &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Shader &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Skin &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Technique &rhs )
{
	return lhs;
}

std::ostream& operator<<( std::ostream &lhs, const Texture &rhs )
{
	return lhs;
}

} // namespace gltf


