-- PostgreSQL physical design for the indexed Join Order Benchmark profile.
--
-- The source IMDb schema declares every `id` column as a primary key.  The
-- benchmark loader used by BloomPG imports heap tables without constraints,
-- so recreate those key access paths explicitly and add one ordinary B-tree
-- for every schema-level foreign-key/join column.  These indexes describe the
-- data model; they are not tailored to an individual JOB query.

CREATE UNIQUE INDEX IF NOT EXISTS aka_name_id_idx ON aka_name (id);
CREATE INDEX IF NOT EXISTS aka_name_person_id_idx ON aka_name (person_id);

CREATE UNIQUE INDEX IF NOT EXISTS aka_title_id_idx ON aka_title (id);
CREATE INDEX IF NOT EXISTS aka_title_movie_id_idx ON aka_title (movie_id);
CREATE INDEX IF NOT EXISTS aka_title_kind_id_idx ON aka_title (kind_id);
CREATE INDEX IF NOT EXISTS aka_title_episode_of_id_idx ON aka_title (episode_of_id);

CREATE UNIQUE INDEX IF NOT EXISTS cast_info_id_idx ON cast_info (id);
CREATE INDEX IF NOT EXISTS cast_info_person_id_idx ON cast_info (person_id);
CREATE INDEX IF NOT EXISTS cast_info_movie_id_idx ON cast_info (movie_id);
CREATE INDEX IF NOT EXISTS cast_info_person_role_id_idx ON cast_info (person_role_id);
CREATE INDEX IF NOT EXISTS cast_info_role_id_idx ON cast_info (role_id);

CREATE UNIQUE INDEX IF NOT EXISTS char_name_id_idx ON char_name (id);
CREATE UNIQUE INDEX IF NOT EXISTS comp_cast_type_id_idx ON comp_cast_type (id);
CREATE UNIQUE INDEX IF NOT EXISTS company_name_id_idx ON company_name (id);
CREATE UNIQUE INDEX IF NOT EXISTS company_type_id_idx ON company_type (id);

CREATE UNIQUE INDEX IF NOT EXISTS complete_cast_id_idx ON complete_cast (id);
CREATE INDEX IF NOT EXISTS complete_cast_movie_id_idx ON complete_cast (movie_id);
CREATE INDEX IF NOT EXISTS complete_cast_subject_id_idx ON complete_cast (subject_id);
CREATE INDEX IF NOT EXISTS complete_cast_status_id_idx ON complete_cast (status_id);

CREATE UNIQUE INDEX IF NOT EXISTS info_type_id_idx ON info_type (id);
CREATE UNIQUE INDEX IF NOT EXISTS keyword_id_idx ON keyword (id);
CREATE UNIQUE INDEX IF NOT EXISTS kind_type_id_idx ON kind_type (id);
CREATE UNIQUE INDEX IF NOT EXISTS link_type_id_idx ON link_type (id);

CREATE UNIQUE INDEX IF NOT EXISTS movie_companies_id_idx ON movie_companies (id);
CREATE INDEX IF NOT EXISTS movie_companies_movie_id_idx ON movie_companies (movie_id);
CREATE INDEX IF NOT EXISTS movie_companies_company_id_idx ON movie_companies (company_id);
CREATE INDEX IF NOT EXISTS movie_companies_company_type_id_idx ON movie_companies (company_type_id);

CREATE UNIQUE INDEX IF NOT EXISTS movie_info_idx_id_idx ON movie_info_idx (id);
CREATE INDEX IF NOT EXISTS movie_info_idx_movie_id_idx ON movie_info_idx (movie_id);
CREATE INDEX IF NOT EXISTS movie_info_idx_info_type_id_idx ON movie_info_idx (info_type_id);

CREATE UNIQUE INDEX IF NOT EXISTS movie_keyword_id_idx ON movie_keyword (id);
CREATE INDEX IF NOT EXISTS movie_keyword_movie_id_idx ON movie_keyword (movie_id);
CREATE INDEX IF NOT EXISTS movie_keyword_keyword_id_idx ON movie_keyword (keyword_id);

CREATE UNIQUE INDEX IF NOT EXISTS movie_link_id_idx ON movie_link (id);
CREATE INDEX IF NOT EXISTS movie_link_movie_id_idx ON movie_link (movie_id);
CREATE INDEX IF NOT EXISTS movie_link_linked_movie_id_idx ON movie_link (linked_movie_id);
CREATE INDEX IF NOT EXISTS movie_link_link_type_id_idx ON movie_link (link_type_id);

CREATE UNIQUE INDEX IF NOT EXISTS name_id_idx ON name (id);
CREATE UNIQUE INDEX IF NOT EXISTS role_type_id_idx ON role_type (id);

CREATE UNIQUE INDEX IF NOT EXISTS title_id_idx ON title (id);
CREATE INDEX IF NOT EXISTS title_kind_id_idx ON title (kind_id);
CREATE INDEX IF NOT EXISTS title_episode_of_id_idx ON title (episode_of_id);

CREATE UNIQUE INDEX IF NOT EXISTS movie_info_id_idx ON movie_info (id);
CREATE INDEX IF NOT EXISTS movie_info_movie_id_idx ON movie_info (movie_id);
CREATE INDEX IF NOT EXISTS movie_info_info_type_id_idx ON movie_info (info_type_id);

CREATE UNIQUE INDEX IF NOT EXISTS person_info_id_idx ON person_info (id);
CREATE INDEX IF NOT EXISTS person_info_person_id_idx ON person_info (person_id);
CREATE INDEX IF NOT EXISTS person_info_info_type_id_idx ON person_info (info_type_id);

ANALYZE;
