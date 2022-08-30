// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmoMeasure.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <numeric>

#include <GL/glew.h>

#if ENABLE_MEASURE_GIZMO

std::string surface_feature_type_as_string(Slic3r::Measure::SurfaceFeatureType type)
{
    switch (type)
    {
    default:
    case Slic3r::Measure::SurfaceFeatureType::Undef:  { return L("Undefined"); }
    case Slic3r::Measure::SurfaceFeatureType::Point:  { return L("Point"); }
    case Slic3r::Measure::SurfaceFeatureType::Edge:   { return L("Edge"); }
    case Slic3r::Measure::SurfaceFeatureType::Circle: { return L("Circle"); }
    case Slic3r::Measure::SurfaceFeatureType::Plane:  { return L("Plane"); }
    }
}

namespace Slic3r {
namespace GUI {

static const Slic3r::ColorRGBA BASIC_HOVER_COLOR    = { 0.8f, 0.2f, 0.2f, 1.0f };
static const Slic3r::ColorRGBA EXTENDED_HOVER_COLOR = { 0.2f, 0.8f, 0.2f, 1.0f };
static const Slic3r::ColorRGBA LOCK_COLOR           = { 0.5f, 0.5f, 0.5f, 1.0f };

static const int POINT_ID         = 100;
static const int EDGE_ID          = 200;
static const int CIRCLE_ID        = 300;
static const int CIRCLE_CENTER_ID = 301;
static const int PLANE_ID         = 400;

GLGizmoMeasure::GLGizmoMeasure(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
    GLModel::Geometry sphere_geometry = smooth_sphere(16, 7.5f);
    m_sphere.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(sphere_geometry.get_as_indexed_triangle_set())));
    m_sphere.model.init_from(std::move(sphere_geometry));

    GLModel::Geometry cylinder_geometry = smooth_cylinder(16, 5.0f, 1.0f);
    m_cylinder.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(cylinder_geometry.get_as_indexed_triangle_set())));
    m_cylinder.model.init_from(std::move(cylinder_geometry));
}

bool GLGizmoMeasure::on_mouse(const wxMouseEvent &mouse_event)
{
    m_mouse_pos_x = mouse_event.GetX();
    m_mouse_pos_y = mouse_event.GetY();

    if (mouse_event.Moving()) {
        // only for sure 
        m_mouse_left_down = false;
        return false;
    }
    else if (mouse_event.LeftDown()) {
        if (m_hover_id != -1) {
            m_mouse_left_down = true;            
            return true;
        }

        // fix: prevent restart gizmo when reselect object
        // take responsibility for left up
        if (m_parent.get_first_hover_volume_idx() >= 0)
            m_mouse_left_down = true;
        
    }
    else if (mouse_event.LeftUp()) {
        if (m_mouse_left_down) {
            // responsible for mouse left up after selecting plane
            m_mouse_left_down = false;
            return true;
        }
    }
    else if (mouse_event.Leaving())
        m_mouse_left_down = false;

    return false;
}

void GLGizmoMeasure::data_changed()
{
    const Selection&  selection     = m_parent.get_selection();
    const ModelObject* model_object = nullptr;
    const ModelVolume* model_volume = nullptr;
    if (selection.is_single_full_instance() ||
        selection.is_from_single_object() ) {        
        model_object = selection.get_model()->objects[selection.get_object_idx()];
        model_volume = model_object->volumes[selection.get_first_volume()->volume_idx()];
    }
    if (model_object != m_old_model_object || model_volume != m_old_model_volume)
        update_if_needed();
}

bool GLGizmoMeasure::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (action == SLAGizmoEventType::CtrlDown) {
        if (m_ctrl_kar_filter.is_first()) {
            if (!m_features.empty())
                m_mode = EMode::ExtendedSelection;
        }

        m_ctrl_kar_filter.increase_count();
    }
    else if (action == SLAGizmoEventType::CtrlUp) {
        m_ctrl_kar_filter.reset_count();
        m_mode = EMode::BasicSelection;
    }

    return true;
}

bool GLGizmoMeasure::on_init()
{
    m_shortcut_key = WXK_CONTROL_U;
    return true;
}

void GLGizmoMeasure::on_set_state()
{
    if (m_state == Off)
        m_ctrl_kar_filter.reset_count();
    else
        m_mode = EMode::BasicSelection;
}

CommonGizmosDataID GLGizmoMeasure::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

std::string GLGizmoMeasure::on_get_name() const
{
    return _u8L("Measure");
}

bool GLGizmoMeasure::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA) ?
        selection.is_single_full_instance() :
        selection.is_single_volume() || selection.is_single_volume_instance();
}

void GLGizmoMeasure::on_render()
{
    // do not render if the user is panning/rotating the 3d scene
    if (m_parent.is_mouse_dragging())
        return;

    const Selection& selection = m_parent.get_selection();

    if ((wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA && selection.is_single_full_instance()) ||
        (selection.is_single_volume() || selection.is_single_volume_instance())) {
        update_if_needed();

        const Transform3d& model_matrix = selection.get_first_volume()->world_matrix();
        const Camera& camera = wxGetApp().plater()->get_camera();
        const float inv_zoom = (float)camera.get_inv_zoom();

        Vec3f pos;
        Vec3f normal;
        size_t facet_idx;
        m_c->raycaster()->raycasters().front()->unproject_on_mesh(Vec2d(m_mouse_pos_x, m_mouse_pos_y), model_matrix, camera, pos, normal, nullptr, &facet_idx);

        std::vector<Measure::SurfaceFeature> features;
        if (m_mode == EMode::BasicSelection) {
            std::optional<Measure::SurfaceFeature> feat = m_measuring->get_feature(facet_idx, pos.cast<double>());
            if (feat.has_value())
                features.emplace_back(*feat);
        }

        if (m_mode == EMode::BasicSelection) {
            if (m_features != features) {
                GLGizmoMeasure::on_unregister_raycasters_for_picking();
                m_features = features;
                if (m_features.empty())
                    return;

                for (const Measure::SurfaceFeature& feature : m_features) {
                    switch (feature.get_type()) {
                    case Measure::SurfaceFeatureType::Point:
                    {
                        m_raycasters.insert({ POINT_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID, *m_sphere.mesh_raycaster) });
                        break;
                    }
                    case Measure::SurfaceFeatureType::Edge:
                    {
                        m_raycasters.insert({ EDGE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, EDGE_ID, *m_cylinder.mesh_raycaster) });
                        break;
                    }
                    case Measure::SurfaceFeatureType::Circle:
                    {
                        const auto& [center, radius, n] = feature.get_circle();
                        m_circle.reset();
                        GLModel::Geometry circle_geometry = smooth_torus(64, 16, float(radius), 5.0f * inv_zoom);
                        m_circle.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(circle_geometry.get_as_indexed_triangle_set())));
                        m_circle.model.init_from(std::move(circle_geometry));

                        m_raycasters.insert({ CIRCLE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID, *m_circle.mesh_raycaster) });
                        m_raycasters.insert({ CIRCLE_CENTER_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_CENTER_ID, *m_sphere.mesh_raycaster) });
                        break;
                    }
                    case Measure::SurfaceFeatureType::Plane:
                    {
                        const auto& [idx, normal, pt] = feature.get_plane();
                        const std::vector<std::vector<int>> planes_triangles = m_measuring->get_planes_triangle_indices();
                        const std::vector<int>& triangle_indices = planes_triangles[idx];

                        const indexed_triangle_set its = (m_old_model_volume != nullptr) ? m_old_model_volume->mesh().its : m_old_model_object->volumes.front()->mesh().its;
                        GLModel::Geometry init_data;
                        init_data.format = { GUI::GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
                        unsigned int i = 0;
                        for (int idx : triangle_indices) {
                            const Vec3f& v0 = its.vertices[its.indices[idx][0]];
                            const Vec3f& v1 = its.vertices[its.indices[idx][1]];
                            const Vec3f& v2 = its.vertices[its.indices[idx][2]];

                            const Vec3f n = (v1 - v0).cross(v2 - v0).normalized();
                            init_data.add_vertex(v0, n);
                            init_data.add_vertex(v1, n);
                            init_data.add_vertex(v2, n);
                            init_data.add_triangle(i, i + 1, i + 2);
                            i += 3;
                        }

                        m_plane.reset();
                        m_plane.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(init_data.get_as_indexed_triangle_set())));
                        m_plane.model.init_from(std::move(init_data));
                        m_raycasters.insert({ PLANE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, PLANE_ID, *m_plane.mesh_raycaster) });
                        break;
                    }
                    }
                }
            }
        }

        GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
        if (shader == nullptr)
            return;
    
        shader->start_using();
        shader->set_uniform("emission_factor", 0.25f);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());

        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
        glsafe(::glEnable(GL_DEPTH_TEST));

        const Transform3d& view_matrix = camera.get_view_matrix();

        ColorRGBA color;
        switch (m_mode)
        {
        case EMode::BasicSelection:    { color = BASIC_HOVER_COLOR; break; }
        case EMode::ExtendedSelection: { color = LOCK_COLOR; break; }
        }

        for (const Measure::SurfaceFeature& feature : m_features) {
            switch (feature.get_type()) {
            case Measure::SurfaceFeatureType::Point:
            {
                const Vec3d& position = feature.get_point();
                const Transform3d feature_matrix = model_matrix * Geometry::translation_transform(position) * Geometry::scale_transform(inv_zoom);
                const Transform3d view_model_matrix = view_matrix * feature_matrix;
                shader->set_uniform("view_model_matrix", view_model_matrix);
                const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * feature_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", view_normal_matrix);
                m_sphere.model.set_color(color);
                m_sphere.model.render();
                auto it = m_raycasters.find(POINT_ID);
                if (it != m_raycasters.end() && it->second != nullptr)
                    it->second->set_transform(feature_matrix);
                break;
            }
            case Measure::SurfaceFeatureType::Circle:
            {
                const auto& [center, radius, n] = feature.get_circle();
                // render center
                const Transform3d center_matrix = model_matrix * Geometry::translation_transform(center) * Geometry::scale_transform(inv_zoom);
                const Transform3d center_view_model_matrix = view_matrix * center_matrix;
                shader->set_uniform("view_model_matrix", center_view_model_matrix);
                const Matrix3d center_view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * center_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", center_view_normal_matrix);
                m_sphere.model.set_color(color);
                m_sphere.model.render();
                auto it = m_raycasters.find(CIRCLE_CENTER_ID);
                if (it != m_raycasters.end() && it->second != nullptr)
                    it->second->set_transform(center_matrix);

                const Transform3d circle_matrix = model_matrix * Geometry::translation_transform(center);
                const Transform3d circle_view_model_matrix = view_matrix * circle_matrix;
                shader->set_uniform("view_model_matrix", circle_view_model_matrix);
                const Matrix3d circle_view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * circle_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", circle_view_normal_matrix);
                m_circle.model.set_color(color);
                m_circle.model.render();
                it = m_raycasters.find(CIRCLE_ID);
                if (it != m_raycasters.end() && it->second != nullptr)
                    it->second->set_transform(circle_matrix);
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            {
                const auto& [start, end] = feature.get_edge();
                auto q = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), end - start);
                const Transform3d feature_matrix = model_matrix * Geometry::translation_transform(start) * q *
                    Geometry::scale_transform({ (double)inv_zoom, (double)inv_zoom, (end - start).norm() });
                const Transform3d view_model_matrix = view_matrix * feature_matrix;
                shader->set_uniform("view_model_matrix", view_model_matrix);
                const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * feature_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", view_normal_matrix);
                m_cylinder.model.set_color(color);
                m_cylinder.model.render();
                auto it = m_raycasters.find(EDGE_ID);
                if (it != m_raycasters.end() && it->second != nullptr)
                    it->second->set_transform(feature_matrix);
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                const auto& [idx, normal, pt] = feature.get_plane();
                assert(idx < m_plane_models_cache.size());
                const Transform3d view_model_matrix = view_matrix * model_matrix;
                shader->set_uniform("view_model_matrix", view_model_matrix);
                const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", view_normal_matrix);
                m_plane_models_cache[idx].set_color(color);
                m_plane_models_cache[idx].render();
                auto it = m_raycasters.find(PLANE_ID);
                if (it != m_raycasters.end() && it->second != nullptr)
                    it->second->set_transform(model_matrix);
                break;
            }
            }
        }
        shader->stop_using();
    }
}

void GLGizmoMeasure::update_if_needed()
{
    auto update_plane_models_cache = [this](const indexed_triangle_set& its) {
        m_plane_models_cache.clear();
        const std::vector<std::vector<int>> planes_triangles = m_measuring->get_planes_triangle_indices();
        for (const std::vector<int>& triangle_indices : planes_triangles) {
            m_plane_models_cache.emplace_back(GLModel());
            GLModel::Geometry init_data;
            init_data.format = { GUI::GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
            unsigned int i = 0;
            for (int idx : triangle_indices) {
                const Vec3f& v0 = its.vertices[its.indices[idx][0]];
                const Vec3f& v1 = its.vertices[its.indices[idx][1]];
                const Vec3f& v2 = its.vertices[its.indices[idx][2]];

                const Vec3f n = (v1 - v0).cross(v2 - v0).normalized();
                init_data.add_vertex(v0, n);
                init_data.add_vertex(v1, n);
                init_data.add_vertex(v2, n);
                init_data.add_triangle(i, i + 1, i + 2);
                i += 3;
            }
            m_plane_models_cache.back().init_from(std::move(init_data));
        }
    };

    auto do_update = [this, update_plane_models_cache](const ModelObject* object, const ModelVolume* volume) {
        const indexed_triangle_set& its = (volume != nullptr) ? volume->mesh().its : object->volumes.front()->mesh().its;
        m_measuring.reset(new Measure::Measuring(its));

        update_plane_models_cache(its);

        // Let's save what we calculated it from:
        m_volumes_matrices.clear();
        m_volumes_types.clear();
        m_first_instance_scale = Vec3d::Ones();
        m_first_instance_mirror = Vec3d::Ones();
        if (object != nullptr) {
            for (const ModelVolume* vol : object->volumes) {
                m_volumes_matrices.push_back(vol->get_matrix());
                m_volumes_types.push_back(vol->type());
            }
            m_first_instance_scale  = object->instances.front()->get_scaling_factor();
            m_first_instance_mirror = object->instances.front()->get_mirror();
        }
        m_old_model_object = object;
        m_old_model_volume = volume;
    };

    const ModelObject* mo = m_c->selection_info()->model_object();
    const ModelVolume* mv = m_c->selection_info()->model_volume();
    if (m_state != On || (mo == nullptr && mv == nullptr))
        return;

    if (mo == nullptr)
        mo = mv->get_object();

    if (mo->instances.empty())
        return;

    if (!m_measuring || mo != m_old_model_object || mv != m_old_model_volume || mo->volumes.size() != m_volumes_matrices.size())
        do_update(mo, mv);

    // We want to recalculate when the scale changes - some planes could (dis)appear.
    if (!mo->instances.front()->get_scaling_factor().isApprox(m_first_instance_scale) ||
        !mo->instances.front()->get_mirror().isApprox(m_first_instance_mirror))
        do_update(mo, mv);

    for (unsigned int i = 0; i < mo->volumes.size(); ++i) {
        if (!mo->volumes[i]->get_matrix().isApprox(m_volumes_matrices[i]) ||
            mo->volumes[i]->type() != m_volumes_types[i]) {
            do_update(mo, mv);
            break;
        }
    }
}

void GLGizmoMeasure::on_render_input_window(float x, float y, float bottom_limit)
{
    static Measure::SurfaceFeature last_feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Undef, Vec3d::Zero(), Vec3d::Zero(), std::nullopt, 0.0);

    static float last_y = 0.0f;
    static float last_h = 0.0f;

    m_imgui->begin(_L("Measure tool"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    // adjust window position to avoid overlap the view toolbar
    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    ImGui::SetWindowPos(ImVec2(x, y), ImGuiCond_Always);
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }

    if (m_features.empty())
        m_imgui->text(_u8L("Select features to measure"));

    auto add_row_to_table = [this](const wxString& label, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, label);
        ImGui::TableSetColumnIndex(1);
        m_imgui->text(value);
    };

    const Transform3d volume_matrix = m_parent.get_selection().get_first_volume()->world_matrix();
    Measure::SurfaceFeature curr_feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Undef, Vec3d::Zero(), Vec3d::Zero(), std::nullopt, 0.0);
    for (size_t i = 0; i < m_features.size(); ++i) {
        curr_feature = m_features[i];
        const Measure::SurfaceFeatureType type = curr_feature.get_type();
        if (type != Measure::SurfaceFeatureType::Undef) {
            const std::string header = surface_feature_type_as_string(type) + std::string("##") + std::to_string(i);
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("Data", 2)) {
                    switch (type)
                    {
                    case Measure::SurfaceFeatureType::Point:
                    {
                        const Vec3d position = volume_matrix * curr_feature.get_point();
                        char buf[1024];
                        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", position.x(), position.y(), position.z());
                        add_row_to_table(_u8L("Position") + ":", std::string(buf));
                        break;
                    }
                    case Measure::SurfaceFeatureType::Edge:
                    {
                        auto [from, to] = curr_feature.get_edge();
                        from = volume_matrix * from;
                        to = volume_matrix * to;
                        char buf[1024];
                        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", from.x(), from.y(), from.z());
                        add_row_to_table(_u8L("From") + ":", std::string(buf));
                        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", to.x(), to.y(), to.z());
                        add_row_to_table(_u8L("To") + ":", std::string(buf));
                        break;
                    }
                    case Measure::SurfaceFeatureType::Circle:
                    {
                        auto [center, radius, normal] = curr_feature.get_circle();
                        center = volume_matrix * center;
                        normal = volume_matrix.matrix().block(0, 0, 3, 3).inverse().transpose() * normal;
                        char buf[1024];
                        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", center.x(), center.y(), center.z());
                        add_row_to_table(_u8L("Center") + ":", std::string(buf));
                        sprintf(buf, "%.3f", radius);
                        add_row_to_table(_u8L("Radius") + ":", std::string(buf));
                        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", normal.x(), normal.y(), normal.z());
                        add_row_to_table(_u8L("Normal") + ":", std::string(buf));
                        break;
                    }
                    case Measure::SurfaceFeatureType::Plane:
                    {
                        auto [idx, normal, origin] = curr_feature.get_plane();
                        origin = volume_matrix * origin;
                        normal = volume_matrix.matrix().block(0, 0, 3, 3).inverse().transpose() * normal;
                        char buf[1024];
                        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", origin.x(), origin.y(), origin.z());
                        add_row_to_table(_u8L("Origin") + ":", std::string(buf));
                        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", normal.x(), normal.y(), normal.z());
                        add_row_to_table(_u8L("Normal") + ":", std::string(buf));
                        break;
                    }
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    if (last_feature != curr_feature) {
        // the dialog may have changed its size, ask for an extra frame to render it properly
        last_feature = curr_feature;
        m_imgui->set_requires_extra_frame();
    }

    if (!m_features.empty()) {
            static const std::string ctrl =
#ifdef __APPLE__
                "⌘"
#else
                "Ctrl"
#endif //__APPLE__
                ;

        ImGui::Separator();
        m_imgui->text(_u8L("Press") + " " + ctrl + " " + _u8L("to enable extended selection"));
    }
    m_imgui->end();
}

void GLGizmoMeasure::on_register_raycasters_for_picking()
{
    // the features are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoMeasure::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.set_raycaster_gizmos_on_top(false);
    m_raycasters.clear();
}

} // namespace GUI
} // namespace Slic3r

#endif // ENABLE_MEASURE_GIZMO
