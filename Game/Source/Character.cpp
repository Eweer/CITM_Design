#include "Character.h"
#include "App.h"

// Modules
#include "Render.h"

// Utils
#include "Log.h"

#include <regex>


constexpr uint Character_SIZE = 30;

Character::Character() = default;

Character::Character(pugi::xml_node const &itemNode = pugi::xml_node()) : Entity(itemNode) {}

Character::~Character() = default;

bool Character::Awake()
{
	return true;
}

bool Character::Start()
{
	SpawnEntity();
	InitializeTexture();

	return true;
}

void Character::InitializeTexture() const
{
	if(!texture) return;

	if(!texture->Start(texture->GetCurrentAnimName()))
		LOG("Couldnt start %s anim", texture->GetCurrentAnimName());
	if(texture->GetAnimStyle() == AnimIteration::UNKNOWN)
		texture->SetAnimStyle(AnimIteration::LOOP_FROM_START);
}

bool Character::Update()
{
	//Update Character position in pixels
	position.x = METERS_TO_PIXELS(pBody->body->GetTransform().p.x) - Character_SIZE/2;
	position.y = METERS_TO_PIXELS(pBody->body->GetTransform().p.y) - Character_SIZE/2;

	app->render->DrawTexture(texture->UpdateAndGetFrame(), position.x, position.y);

	if(hp <= 0) Disable();

	return true;
}

bool Character::Pause() const
{
	return app->render->DrawCharacterTexture(
		texture->GetCurrentFrame(),
		iPoint(position.x - colliderOffset.x, position.y - colliderOffset.y),
		(bool)dir,
		texture->GetFlipPivot()
	);
}

bool Character::CleanUp()
{
	texture->CleanUp();
	return true;
}

void Character::SetPaths()
{
	texturePath = parameters.parent().attribute("texturepath").as_string();

	fxPath = parameters.parent().attribute("audiopath").as_string();
	fxPath += parameters.parent().attribute("fxfolder").as_string();

}

void Character::CreatePhysBody() 
{ 
	// <physics>
	auto currentNode = parameters.child("physics");
	
	if(currentNode.empty()) [[unlikely]]
	{
		LOG("Entity %s has no physics node", name.c_str());
		return;
	}
	
	type = (ColliderLayers)currentNode.attribute("colliderlayers").as_int();
	
	float32 gravity = currentNode.attribute("gravityscale") ? currentNode.attribute("gravityscale").as_float() : 1.0f;
	float32 restitution = currentNode.attribute("restitution") ? currentNode.attribute("restitution").as_float() : 1.0f;
		
	// <properties/> (or <animation> if properties doesn't exist
	if(currentNode = currentNode.parent().child("animationdata").first_child();
	   currentNode.empty())
	{
		LOG("No animationdata on %s", name);
		return;
	}

	while(currentNode) 
	{
		if(!currentNode.child("collidergroup").empty()) 
			break;
		currentNode = currentNode.next_sibling();
	}
	
	// <animation> that has the collider child or <null handle> if no node exists
	if(!currentNode)
	{
		LOG("Entity %s has no collider node", name.c_str());
		return;
	}
	
	// <collidergroup>
	for(auto const &colliderGroupNode : currentNode.children("collidergroup"))
	{
		iPoint width_height(
			colliderGroupNode.attribute("width").as_int(),
			colliderGroupNode.attribute("height").as_int()
		);

		if(!pBody)
		{
			colliderOffset = {
				colliderGroupNode.first_child().attribute("x").as_int(),
				colliderGroupNode.first_child().attribute("y").as_int()
			};
			
			auto bodyType = BodyTypeStrToEnum(colliderGroupNode.attribute("class").as_string());
			
			auto bodyPtr = app->physics->CreateBody(
				position + colliderOffset,
				bodyType,
				0.0f,
				{0.00f,0.01f},
				gravity
			);

			auto pBodyPtr = app->physics->CreatePhysBody(
				bodyPtr,
				width_height,
				type
			);

			pBody = std::move(pBodyPtr);
			pBody->listener = this;
		}
		
		for(auto const &elem : colliderGroupNode.children())
		{
			bool bSensor = currentNode.attribute("sensor").as_bool();
			float32 density = currentNode.attribute("density") ? currentNode.attribute("density").as_float() : 0.0f;

			// iterate over digits in node and add them to a b2Vec2 as x, y.
			// Will be used on shape creation
			
			std::vector<b2Vec2> tempData;
			std::string shapeType = elem.name();

			if(StrEquals(shapeType, "chain") || StrEquals(shapeType, "polygon"))
			{
				const std::string xyStr = elem.attribute("points").as_string();
				static const std::regex r(R"((-?\d{1,3})(?:\.\d+)*,(-?\d{1,3})(?:\.\d+)*)");
				auto xyStrBegin = std::sregex_iterator(xyStr.begin(), xyStr.end(), r);
				auto xyStrEnd = std::sregex_iterator();

				for(std::sregex_iterator i = xyStrBegin; i != xyStrEnd; ++i)
				{
					std::smatch match = *i;
					tempData.push_back(
						{
							PIXEL_TO_METERS(stoi(match[1].str())),
							PIXEL_TO_METERS(stoi(match[2].str()))
						}
					);
				}
			}
			else if(StrEquals(shapeType, "rectangle"))
			{
				tempData.push_back(
					{
						PIXEL_TO_METERS(width_height.x),
						PIXEL_TO_METERS(width_height.y)
					}
				);
			}
			else if(StrEquals(shapeType, "circle"))
			{
				tempData.push_back(
					{
						colliderGroupNode.attribute("radius").as_float(),
						0
					}
				);
			}

			// If there was no points, xml is malformed
			// We continue the loop to not crash the game
			if(tempData.empty()) continue;

			// Create the Shape
			ShapeData shape(shapeType, tempData);

			// Fix position if shape is a circle
			b2Vec2 fixPos(0, 0);
			if(shape.shape.get()->GetType() == b2Shape::e_circle)
			{
				fixPos = PIXEL_TO_METERS(
					{
						elem.attribute("x").as_int() - colliderOffset.x,
						elem.attribute("y").as_int() - colliderOffset.y
					}
				);
			}

			float32 friction = elem.attribute("friction") ? elem.attribute("friction").as_float() : 1.0f;
			uint16 maskFlag = SetMaskFlag(name, colliderGroupNode, elem);

			auto fixtureDef = app->physics->CreateFixtureDef(
				shape,
				static_cast<uint16>(type),
				maskFlag,
				bSensor,
				density,
				friction,
				restitution,
				fixPos
			);

			auto fixturePtr = pBody->body->CreateFixture(fixtureDef.get());

			if(StrEquals(elem.attribute("name").as_string(), "ground"))
			{
				pBody->ground = std::make_unique<FixtureData>(
					std::string(elem.attribute("name").as_string()),
					fixturePtr
				);
			}
		}
	}
}

uint16 Character::SetMaskFlag(std::string_view name, pugi::xml_node const &colliderGroupNode, pugi::xml_node const &colliderNode) const
{
	uint16 maskFlag = 0x0001;
	if(StrEquals(name, "player"))
	{
		using enum ColliderLayers;
		if(StrEquals(colliderGroupNode.attribute("name").as_string(), "CharacterSensor"))
			maskFlag = static_cast<uint16>(ENEMIES | TRIGGERS | CHECKPOINTS);
		else if(StrEquals(colliderGroupNode.attribute("name").as_string(), "Terrain"))
			maskFlag = static_cast<uint16>(PLATFORMS | ITEMS);
	}
	return maskFlag;
}

void Character::BeforeCollisionStart(b2Fixture *fixtureA, b2Fixture *fixtureB, PhysBody *pBodyA, PhysBody *pBodyB)
{
	/* To override */
}

void Character::OnCollisionStart(b2Fixture *fixtureA, b2Fixture *fixtureB, PhysBody *pBodyA, PhysBody *pBodyB)
{
	/* To override */
}

bool Character::CreateEntityPath(std::string &entityFolder) const
{
	if(!parameters.attribute("class").empty())
		entityFolder = std::string(parameters.attribute("class").as_string()) + "/";

	if(!parameters.attribute("name").empty())
		entityFolder = std::string(parameters.attribute("name").as_string()) + "/" + entityFolder;

	if(!parameters.parent().attribute("texturepath").empty())
		entityFolder = std::string(parameters.parent().attribute("texturepath").as_string()) + entityFolder;

	if(entityFolder.empty())
	{
		LOG("No animation folder specified for %s", name);
		return false;
	}

	return true;
}

void Character::AddTexturesAndAnimationFrames()
{
	texture = std::make_unique<Animation>();
	
	std::string entityFolder = "";
	if(!CreateEntityPath(entityFolder)) return;
	
	auto animDataNode = parameters.child("animationdata");
	
	textureOffset = {
		.x = animDataNode.child("properties").attribute("pivotx").as_int(),
		.y = animDataNode.child("properties").attribute("pivoty").as_int()
	};

	texture->setPivot(textureOffset);
	
	struct dirent **folderList;
	const char *dirPath = entityFolder.c_str();
	int nCharacterFolder = scandir(dirPath, &folderList, nullptr, DescAlphasort);
	
	if(nCharacterFolder < 0) return;

	//for each file/folder in Character folder
	while(nCharacterFolder--)
	{
		if(folderList[nCharacterFolder]->d_name[0] == '.')
		{
			free(folderList[nCharacterFolder]);
			continue;
		}
		struct dirent **nameList;
		std::string animationPath = entityFolder + std::string(folderList[nCharacterFolder]->d_name) + "/";
		int nAnimationContents = scandir(animationPath.c_str(), &nameList, nullptr, DescAlphasort);
		
		if(nAnimationContents < 0) break;

		//for each file in subfolders of Character folder
		while(nAnimationContents--)
		{
			if(nameList[nAnimationContents]->d_name[0] == '.')
			{
				free(nameList[nAnimationContents]);
				continue;
			}
			
			std::string frameName = nameList[nAnimationContents]->d_name;
			std::string framesPath = animationPath + std::string(nameList[nAnimationContents]->d_name);

			LOG("Loaded %s.", framesPath.c_str());

			auto action = std::string(folderList[nCharacterFolder]->d_name);
			action[0] = std::tolower(action[0], std::locale());

			//if it's not the first frame with such name we continue looping
			if(texture->AddFrame(framesPath.c_str(), action) != 1) [[likely]]
				continue;

			//if it's the first frame we set the action animation parameters 
			// (or default them in case they don't exist)
			SetAnimationParameters(animDataNode, action);

			free(nameList[nAnimationContents]);
		}
		free(nameList);
		free(folderList[nCharacterFolder]);
	}
	free(folderList);
}

void Character::SetAnimationParameters(pugi::xml_node const &animDataNode, std::string const &action)
{
	auto animationParameters = animDataNode.find_child_by_attribute("name", action.c_str());

	if(!animationParameters.empty() && animationParameters.attribute("speed"))
		texture->SetSpeed(animationParameters.attribute("speed").as_float());
	else
		texture->SetSpeed(0.2f);

	if(!animationParameters.empty() && animationParameters.attribute("style"))
		texture->SetAnimStyle(static_cast<AnimIteration>(animationParameters.attribute("animstyle").as_int()));
	else
		texture->SetAnimStyle(AnimIteration::LOOP_FROM_START);

}

/*
 *	Let's just say that today is november 24th. Tomorrow is my birthday. It's 4.13 AM. I can't even think.
 *	BUT I'm just gonna leave this here and hope that I can fix it later.
 *	I'm so sorry about this. But hey, at least it works.
 *	If you have any issue don't hesitate to contact me. I'll be happy to help you.
 *	r(R"((([a-zA-Z]+(?:_??(?:(?!(?:_image|_static|(?:_*?anim\d+)|\d+)(?:\.png|\.jpg)))[a-zA-Z]*))_?(?:(image|static|(?:anim\d+)|\d+))(\d+)*(?:\.png|\.jpg)))")
 *
 *  haHA it didn't work. Improved version. Only took 8 minutes to fix it. I'm so proud of myself.
 *  Overall I've been working on this regex for 3 hours and a half. I'm surprised it took so little.
 *  https://regex101.com/r/8CRsUh/1
 *  https://regex101.com/r/viuXac/2
 *  final ? https://regex101.com/r/pdLoYK/1
 *  https://regex101.com/r/VDIgWc/1
 *  static const std::regex r(R"((([a-zA-Z]+(?:_??(?:(?!(?:_image|_static|(?:_*?anim\d+)|\d+)(?:\.png|\.jpg)))[a-zA-Z]*))_?
 *	 						  (?:(image|static|(?:anim(?:\d+)*?)|\d+))(\d+)*(?:\.png|\.jpg))))")  
 *  and im not even gonna use it.........................
*/
