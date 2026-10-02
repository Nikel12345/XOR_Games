#pragma once
#include <SDL3/SDL.h>
#include "Engine.h"
#include "InputManager.h"
#include "GravitySystem.h"

class Game {
public:
	Game(Engine* engine);
	SDL_AppResult MainInit();
	SDL_AppResult MainIterate();

private:
	Engine* engine = nullptr;

	ObjectManager* objectManager;
	CameraManager* cameraManager;
	InputManager* input;

	EngineContext* ctx;
	std::vector<InputManager::KeyEvent> key_events_scratch;
	std::vector<SDL_Scancode> held_keys_scratch;

	void UpdateKeyboardControls();
	// Наведение курсора на UI: пересечение курсора с ректом узла → второй albedo, иначе первый.
	void UpdateUIHover();

	GravitySystem gravity;

	float width;
	float height;
	float mouse_x = 0;
	float mouse_y = 0;
};
