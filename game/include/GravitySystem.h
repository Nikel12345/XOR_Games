#pragma once
#include <cstdint>
#include <vector>

class ObjectManager;
struct SceneData;

class GravitySystem {
public:
	void Tick(ObjectManager* om);

private:
	// Гравитация к центрам-сущностям (GravityComponent) + интеграция позиций скоростями.
	// Кубы джетов (JetComponent) гравитация не тянет — их только двигает интеграция.
	void SimulateGravity(ObjectManager* om, SceneData* scene, float sim_dt);
	// Джеты: куб, ушедший от своего центра дальше return_distance, возвращается в центр
	// с прежней скоростью. Центры берёт из gravity_sources — звать после SimulateGravity.
	void ReturnJets(ObjectManager* om, SceneData* scene, float return_distance);

	// Снимок центров тяжести на тик: сначала собираем их (их единицы), потом один проход по
	// притягиваемым. Член, а не локальная переменная, — чтобы не аллоцировать каждый тик.
	// id — GravityComponent::id: по нему ReturnJets находит объекты центра.
	// r2_floor — нижний порог r^2 в формуле ускорения: квадрат core_radius (не меньше защиты от
	// деления на ~0). Ниже порога ускорение линейно по r — см. GravityComponent::core_radius.
	struct GravitySource { float x, y, z, gm; uint32_t id; float r2_floor; };
	std::vector<GravitySource> gravity_sources;
};
