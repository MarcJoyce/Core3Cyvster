/*
 * ObjectControllerImplementation.cpp
 *
 *  Created on: 11/08/2009
 *      Author: victor
 */

#include "server/zone/managers/objectcontroller/ObjectController.h"
#include "server/zone/managers/objectcontroller/command/CommandConfigManager.h"
#include "server/zone/managers/objectcontroller/command/CommandList.h"
#include "server/zone/managers/skill/SkillModManager.h"
#include "server/zone/objects/creature/CreatureObject.h"
#include "server/zone/objects/player/PlayerObject.h"

void ObjectControllerImplementation::loadCommands() {
	configManager = new CommandConfigManager(server);
	queueCommands = new CommandList();

	info(true) << "Loading Queue Commands...";

	configManager->registerSpecialCommands(queueCommands);
	configManager->loadSlashCommandsFile();

	info(true) << "Loaded " << queueCommands->size() << " total commands";

	adminLog.setLoggingName("AdminCommands");

	StringBuffer fileName;
	fileName << "log/admin/admin.log";
	adminLog.setFileLogger(fileName.toString(), true);
	adminLog.setLogging(true);

	// LUA
	/*init();
	Luna<LuaCreatureObject>::Register(L);

	runFile("scripts/testscript.lua");*/
}

void ObjectControllerImplementation::finalize() {
	configManager = nullptr;
	queueCommands = nullptr;
}

bool ObjectControllerImplementation::transferObject(SceneObject* objectToTransfer, SceneObject* destinationObject, int containmentType, bool notifyClient, bool allowOverflow) {
	ManagedReference<SceneObject*> parent = objectToTransfer->getParent().get();

	if (parent == nullptr) {
		error("objectToTransfer parent is nullptr in ObjectManager::transferObject");
		return false;
	}

	uint32 oldContainmentType = objectToTransfer->getContainmentType();

	if (!destinationObject->transferObject(objectToTransfer, containmentType, notifyClient, allowOverflow)) {
		StringBuffer msg;
		msg << "could not add " << objectToTransfer->getLoggingName() << " to this object in ObjectManager::transferObject ";
		msg << "with containmentType: " << containmentType << " allowOverflow: " << allowOverflow << " destination container size:";
		msg << destinationObject->getContainerObjectsSize() << " slotted container size:" << destinationObject->getSlottedObjectsSize();

		destinationObject->error(msg.toString());

		parent->transferObject(objectToTransfer, oldContainmentType);

		return false;
	}

	return true;
}

float ObjectControllerImplementation::activateCommand(CreatureObject* object, unsigned int actionCRC, unsigned int actionCount, uint64 targetID, const UnicodeString& arguments) const {
	// Pre: object is wlocked
	// Post: object is wlocked

	const QueueCommand* queueCommand = getQueueCommand(actionCRC);

	float durationTime = 0.f;

	if (queueCommand == nullptr) {
		object->error() << "unregistered queue command 0x" << hex << actionCRC << " arguments: " << arguments.toString();

		return 0.f;
	}

	float commandTime = queueCommand->getCommandDuration(object, arguments);

	// object->info(true) << "activating queue command -- Name: " << queueCommand->getQueueCommandName() << " arguments = " << arguments.toString() << " Duration: " << commandTime;

	const String& characterAbility = queueCommand->getCharacterAbility();

	if (characterAbility.length() > 1) {
		object->debug() << "activating characterAbility " << characterAbility;

		if (object->isPlayerCreature()) {
			Reference<PlayerObject*> playerObject =  object->getSlottedObject("ghost").castTo<PlayerObject*>();

			if (!playerObject->hasAbility(characterAbility)) {
				object->clearQueueAction(actionCount, 0, 2);

				return 0.f;
			}
		}
	}

	uint32 commandGroup = queueCommand->getCommandGroup();

	if (commandGroup != 0) {
		if (commandGroup == 0xe1c9a54a && queueCommand->getQueueCommandName() != "attack") {
			if (!object->isAiAgent()) {
				object->clearQueueAction(actionCount, 0, 2);

				return 0.f;
			}
		}
	}

	if (queueCommand->requiresAdmin()) {
		try {
			if (object->isPlayerCreature()) {
				Reference<PlayerObject*> ghost = object->getSlottedObject("ghost").castTo<PlayerObject*>();

				if (ghost == nullptr || !ghost->hasGodMode() || !ghost->hasAbility(queueCommand->getQueueCommandName())) {
					adminLog.warning() << object->getDisplayedName() << " attempted to use the '/" << queueCommand->getQueueCommandName() << "' command without permissions";

					object->sendSystemMessage("@error_message:insufficient_permissions");
					object->clearQueueAction(actionCount, 0, 2);

					return 0.f;
				}
			} else {
				return 0.f;
			}

			logAdminCommand(object, queueCommand, targetID, arguments);
		} catch (const Exception& e) {
			Logger::error("Unhandled Exception logging admin commands" + e.getMessage());
		}
	}

	/// Add Skillmods if any
	for (int i = 0; i < queueCommand->getSkillModSize(); ++i) {
		String skillMod;
		int value = queueCommand->getSkillMod(i, skillMod);
		object->addSkillMod(SkillModManager::ABILITYBONUS, skillMod, value, false);
	}

	int errorNumber = queueCommand->doQueueCommand(object, targetID, arguments);

	/// Remove Skillmods if any
	for (int i = 0; i < queueCommand->getSkillModSize(); ++i) {
		String skillMod;
		int value = queueCommand->getSkillMod(i, skillMod);
		object->addSkillMod(SkillModManager::ABILITYBONUS, skillMod, -value, false);
	}

	//onFail onComplete must clear the action from client queue
	if (errorNumber != QueueCommand::SUCCESS) {
		queueCommand->onFail(actionCount, object, errorNumber);
		return 0;
	} else {
		if (queueCommand->getDefaultPriority() != QueueCommand::IMMEDIATE) {
			durationTime = commandTime;
		}

		// info(true) << "calling onComplete from activateCommand with actionCount: " << actionCount << " Duration Time: " << durationTime;

		queueCommand->onComplete(actionCount, object, durationTime);
	}

	// info(true) << "activateCommand -- Final Time = " << durationTime;

	return durationTime;
}

void ObjectControllerImplementation::addQueueCommand(QueueCommand* command) {
	queueCommands->put(command);
}

const QueueCommand* ObjectControllerImplementation::getQueueCommand(const String& name) const {
	return queueCommands->getSlashCommand(name);
}

const QueueCommand* ObjectControllerImplementation::getQueueCommand(uint32 crc) const {
	return queueCommands->getSlashCommand(crc);
}

void ObjectControllerImplementation::logAdminCommand(SceneObject* object, const QueueCommand* queueCommand, uint64 targetID, const UnicodeString& arguments) const {
	String name = "unknown";

	Reference<SceneObject*> targetObject = Core::getObjectBroker()->lookUp(targetID).castTo<SceneObject*>();

	if (targetObject != nullptr) {
		name = targetObject->getDisplayedName();

		if(targetObject->isPlayerCreature())
			name += "(Player)";
		else
			name += "(NPC)";
	} else {
		name = "(null)";
	}

	adminLog.info() << object->getDisplayedName() << " used '/" << queueCommand->getQueueCommandName() << "' on " << name << " with params '" << arguments.toString() << "'";
}
